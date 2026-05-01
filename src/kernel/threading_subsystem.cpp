#include "threading_subsystem.hpp"

namespace cortos
{

ThreadControlBlock::ThreadControlBlock(uint32_t id, Thread::Priority priority, CoreAffinity affinity, std::span<std::byte> stack, Thread::EntryFn&& entry)
   : id(id), base_priority(priority), effective_priority(priority), affinity(affinity), stack(stack), entry(std::move(entry))
{
   cortos_port_context_init(context(), stack.data(), stack.size(), thread_launcher, this);
}

void ThreadControlBlock::prepare_block(std::span<Waitable* const> waitables)
{
   CORTOS_ASSERT(state == State::Running);
   CORTOS_ASSERT(waitables.size() > 0);
   CORTOS_ASSERT(waitables.size() <= config::MAX_WAIT_NODES);

   wait_group.begin(waitables.size());

   // Allocate and enqueue nodes
   for (std::size_t i = 0; auto* waitable : waitables) {
      CORTOS_ASSERT(waitable != nullptr);

      WaitNode* wait_node = wait_nodes.alloc(wait_group, *waitable, i++);
      CORTOS_ASSERT(wait_node != nullptr);

      waitable->add(*wait_node);
   }

   state = State::Blocked;
}

void ThreadControlBlock::notify_block(std::span<Waitable* const> waitables) const
{
   // Snapshot once: all blocks are given the same snapshot. This disregards
   // all side-effects on_thread_blocked invocations have on the effective priority.
   auto const waiter = create_waiter();

   for (auto* waitable : waitables) {
      waitable->on_thread_blocked(waiter);
   }
}

Waitable::Result ThreadControlBlock::commence_block()
{
   cortos_port_pend_reschedule();

   // When we resume, winner info is in wait_group
   return Waitable::Result{
      .index    = wait_group.winner_index,
      .acquired = wait_group.acquired,
   };
}

void ThreadControlBlock::teardown_wait_group(WaitGroup& group) noexcept
{
   // Snapshot once: all removals in this teardown relate to the same waiter (this thread).
   auto const waiter = create_waiter();
   WaitableRefVector<config::MAX_WAIT_NODES> waitables;

   // First pass: collect involved waitables
   wait_nodes.for_each_active([&](WaitNode& node) {
      if (node.group != &group) return;
      if (node.waitable) {
         waitables.push(node.waitable);
      }
   }, &group);

   {
      WaitableGroupLock lock_group(waitables);

      // Second pass: unlink and free under lock
      wait_nodes.for_each_active([&](WaitNode& node) {
         if (node.group != &group) return;

         if (node.waitable) {
            node.waitable->remove(node);
         }
         wait_nodes.free(node);
      }, &group);
   }

   // Third pass: hooks after unlock
   for (auto* waitable : waitables) {
      waitable->on_thread_removed(waiter);
   }
}


StackLayout::StackLayout(std::span<std::byte> const buffer, std::size_t const tls_bytes)
{
   auto const base = reinterpret_cast<std::uintptr_t>(buffer.data());
   auto const end  = base + buffer.size();

   // TCB at very top, aligned down
   auto const tcb_start = align_down(end - sizeof(ThreadControlBlock), alignof(ThreadControlBlock));
   tcb = reinterpret_cast<ThreadControlBlock*>(tcb_start);

   // TLS just below TCB
   auto const tls_size = align_up(tls_bytes, alignof(std::max_align_t));
   auto const tls_top  = tcb_start;
   auto const tls_base = align_down(tls_top - tls_size, alignof(std::max_align_t));

   CORTOS_ASSERT_OP(tls_base, >=, base); // Buffer too small for TLS+TCB

   auto const tls_offset = static_cast<std::size_t>(tls_base - base);
   auto const tls_length = static_cast<std::size_t>(tls_top  - tls_base);
   tls_region = buffer.subspan(tls_offset, tls_length); // zero-length span if tls_bytes == 0

   // User stack: everything below TLS
   auto const stack_len = static_cast<std::size_t>(tls_base - base);
   CORTOS_ASSERT_OP(stack_len, >, 64); // Buffer too small after carving TCB/TLS

   user_stack = buffer.subspan(0, stack_len);
}


void ThreadReadyQueue::push_back(ThreadControlBlock& tcb) noexcept
{
   CORTOS_ASSERT(!tcb.is_enqueued());
   CORTOS_ASSERT_OP(tcb.state, ==, ThreadControlBlock::State::Ready); // Thread must be ready to be enqueued
   tcb.next = nullptr;
   tcb.prev = tail;
   if (tail) tail->next = &tcb; else head = &tcb;
   tail = &tcb;
}

ThreadControlBlock* ThreadReadyQueue::pop_front() noexcept
{
   if (empty()) return nullptr;
   auto* tcb = head;
   head = tcb->next;
   if (head) head->prev = nullptr; else tail = nullptr;
   tcb->next = tcb->prev = nullptr;
   return tcb;
}

void ThreadReadyQueue::remove(ThreadControlBlock& tcb) noexcept
{
   CORTOS_ASSERT(&tcb == head || tcb.prev != nullptr);
   CORTOS_ASSERT(&tcb == tail || tcb.next != nullptr);

   if (tcb.prev) tcb.prev->next = tcb.next; else head = tcb.next;
   if (tcb.next) tcb.next->prev = tcb.prev; else tail = tcb.prev;
   tcb.next = tcb.prev = nullptr;
   // Invariant: if one end is null, both are null
   CORTOS_ASSERT((head == nullptr) == (tail == nullptr));
}


void ThreadReadyMatrix::enqueue_thread(ThreadControlBlock& tcb) noexcept
{
   CORTOS_ASSERT_OP(tcb.effective_priority, <, config::MAX_PRIORITIES);
   CORTOS_ASSERT_OP(tcb.state, ==, ThreadControlBlock::State::Ready); // Can only enqueue Ready threads!
   matrix[tcb.effective_priority].push_back(tcb);
   bitmap |= (1u << tcb.effective_priority);
}

ThreadControlBlock* ThreadReadyMatrix::pop_best_thread() noexcept
{
   if (bitmap == 0) return nullptr;
   auto const priority = std::countr_zero(bitmap);
   ThreadControlBlock* tcb = matrix[priority].pop_front();
   if (matrix[priority].empty()) bitmap &= ~(1u << priority);
   return tcb;
}

void ThreadReadyMatrix::remove_thread(ThreadControlBlock& tcb) noexcept
{
   auto const priority = tcb.effective_priority;
   matrix[priority].remove(tcb);
   if (matrix[priority].empty()) bitmap &= ~(1u << priority);
}

} // namespace cortos
