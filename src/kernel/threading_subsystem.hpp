#ifndef CORTOS_THREADING_SUBSYSTEM_HPP
#define CORTOS_THREADING_SUBSYSTEM_HPP

#include <cortos/kernel/waitable.hpp>
#include <cortos/config/config.hpp>
#include <cortos/port/port.h>

#include "align.hpp"
#include "waitable_utilities.hpp"
#include "wait_subsystem.hpp"

#include <bitset>
#include <limits>

namespace cortos
{

void thread_launcher(void* tcb_ptr);

class ThreadTermination : public Waitable
{
   std::atomic<bool> terminated{false};

public:
   [[nodiscard]] bool has_terminated()
   {
      return terminated.load(std::memory_order_acquire);
   }

   void terminate()
   {
      bool expected = false;
      CORTOS_ASSERT(terminated.compare_exchange_strong(expected, true, std::memory_order_release));

      signal_all(false);
   }
};

struct TaskControlBlock
{
   enum class State : uint8_t { Ready, Running, Blocked, Terminated };
   State state{State::Ready};

   // Intrusive 'linked-list' links for a TaskReadyQueue
   TaskControlBlock* next{nullptr};
   TaskControlBlock* prev{nullptr};

   uint32_t id;
   uint8_t base_priority;
   uint8_t effective_priority; // Can change dynamically

   // Core pinning
   std::uint32_t pinned_core{0};
   CoreAffinity  affinity;

   std::span<std::byte> stack;
   Thread::EntryFn entry;

   WaitGroup wait_group{};
   WaitNodePool wait_nodes{this};

   // Thread-joining waitable
   ThreadTermination termination;

   // Opaque, in-place port context storage
   alignas(CORTOS_PORT_CONTEXT_ALIGN) std::array<std::byte, CORTOS_PORT_CONTEXT_SIZE> context_storage{};
   [[nodiscard]] constexpr auto*       context()       noexcept { return reinterpret_cast<cortos_port_context_t*      >(context_storage.data()); }
   [[nodiscard]] constexpr auto const* context() const noexcept { return reinterpret_cast<cortos_port_context_t const*>(context_storage.data()); }

   [[nodiscard]] constexpr bool is_enqueued() const noexcept
   {
      return next != nullptr || prev != nullptr;
   }

   [[nodiscard]] constexpr bool is_higher_priority_than(TaskControlBlock& rhs)  const noexcept
   {
      return effective_priority < rhs.effective_priority;
   }
   [[nodiscard]] constexpr bool is_higher_priority_than(uint8_t priority_level) const noexcept
   {
      return effective_priority < priority_level;
   }

   TaskControlBlock(uint32_t id, Thread::Priority priority, CoreAffinity affinity, std::span<std::byte> stack, Thread::EntryFn&& entry) :
      id(id), base_priority(priority), effective_priority(priority), affinity(affinity), stack(stack), entry(std::move(entry))
   {
      cortos_port_context_init(context(), stack.data(), stack.size(), thread_launcher, this);
   }

   void prepare_block(std::span<Waitable* const> waitables)
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

   void notify_block(std::span<Waitable* const> waitables) const
   {
      // Snapshot once: all blocks are given the same snapshot. This disregards
      // all side-effects on_thread_blocked invocations have on the effective priority.
      auto const waiter = create_waiter();

      for (auto* waitable : waitables) {
         waitable->on_thread_blocked(waiter);
      }
   }

   Waitable::Result commence_block()
   {
      cortos_port_pend_reschedule();

      // When we resume, winner info is in wait_group
      return Waitable::Result{
         .index    = wait_group.winner_index,
         .acquired = wait_group.acquired,
      };
   }

   void teardown_wait_group(WaitGroup& group) noexcept
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

   [[nodiscard]] Waitable::Waiter create_waiter() const
   {
      return {
         .id                 = id,
         .base_priority      = base_priority,
         .effective_priority = effective_priority,
         .pinned_core        = pinned_core,
         .affinity           = affinity,
      };
   }
};


/**
 * Carves a user-provided buffer region into:
 * +----------------------+ <-- buffer's end (high address)
 * +   TaskControlBlock   + (Fixed size)
 * +----------------------+
 * + Thread-local storage + (Variable size)
 * +----------------------+
 * +     User's stack     +
 * +----------------------+ <-- buffer's base (low address)
 */
struct StackLayout
{
   TaskControlBlock* tcb;
   std::span<std::byte> tls_region;
   std::span<std::byte> user_stack;

   explicit StackLayout(std::span<std::byte> const buffer, std::size_t const tls_bytes)
   {
      auto const base = reinterpret_cast<std::uintptr_t>(buffer.data());
      auto const end  = base + buffer.size();

      // TCB at very top, aligned down
      auto const tcb_start = align_down(end - sizeof(TaskControlBlock), alignof(TaskControlBlock));
      tcb = reinterpret_cast<TaskControlBlock*>(tcb_start);

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
};

class TaskReadyQueue
{
private:
   TaskControlBlock* head{nullptr};
   TaskControlBlock* tail{nullptr};

public:
   [[nodiscard]] constexpr bool empty() const noexcept
   {
      return !head;
   }

   [[nodiscard]] constexpr bool has_peer() const noexcept
   {
      return head && head != tail;
   }

   [[nodiscard]] constexpr TaskControlBlock* front() const noexcept
   {
      return head;
   }

   // This walks the linked list so isn't 'free'
   [[nodiscard]] constexpr std::size_t size() const noexcept
   {
      std::size_t n = 0;
      for (auto* tcb = head; tcb; tcb = tcb->next) ++n;
      return n;
   }

   void push_back(TaskControlBlock& tcb) noexcept
   {
      CORTOS_ASSERT(!tcb.is_enqueued());
      CORTOS_ASSERT_OP(tcb.state, ==, TaskControlBlock::State::Ready); // Task must be ready to be enqueued
      tcb.next = nullptr;
      tcb.prev = tail;
      if (tail) tail->next = &tcb; else head = &tcb;
      tail = &tcb;
   }

   TaskControlBlock* pop_front() noexcept
   {
      if (empty()) return nullptr;
      auto* tcb = head;
      head = tcb->next;
      if (head) head->prev = nullptr; else tail = nullptr;
      tcb->next = tcb->prev = nullptr;
      return tcb;
   }

   void remove(TaskControlBlock& tcb) noexcept
   {
      CORTOS_ASSERT(&tcb == head || tcb.prev != nullptr);
      CORTOS_ASSERT(&tcb == tail || tcb.next != nullptr);

      if (tcb.prev) tcb.prev->next = tcb.next; else head = tcb.next;
      if (tcb.next) tcb.next->prev = tcb.prev; else tail = tcb.prev;
      tcb.next = tcb.prev = nullptr;
      // Invariant: if one end is null, both are null
      CORTOS_ASSERT((head == nullptr) == (tail == nullptr));
   }
};

class TaskReadyMatrix
{
private:
   static constexpr std::size_t BITMAP_BITS = std::numeric_limits<uint32_t>::digits;
   std::array<TaskReadyQueue, config::MAX_PRIORITIES> matrix{};
   uint32_t bitmap{0};
   static_assert(config::MAX_PRIORITIES <= BITMAP_BITS, "bitmap cannot hold that many priorities!");

public:
   constexpr TaskReadyMatrix() = default;

   [[nodiscard]] constexpr int best_priority() const noexcept
   {
      return bitmap ? std::countr_zero(bitmap) : -1;
   }

   [[nodiscard]] constexpr bool empty() const noexcept
   {
      return bitmap == 0;
   }

   [[nodiscard]] constexpr bool empty_at(uint32_t priority) const noexcept
   {
      return matrix[priority].empty();
   }

   [[nodiscard]] constexpr bool has_peer(uint32_t priority) const noexcept
   {
      return matrix[priority].has_peer();
   }

   [[nodiscard]] constexpr std::size_t size_at(uint32_t priority) const noexcept
   {
      return matrix[priority].size();
   }

   [[nodiscard]] constexpr std::bitset<BITMAP_BITS> bitmap_view() const noexcept
   {
      return {bitmap};
   }

   [[nodiscard]] constexpr TaskControlBlock* peek_best_task() const noexcept
   {
      if (bitmap == 0) return nullptr;
      auto const priority = std::countr_zero(bitmap);
      return matrix[priority].front();
   }

   void enqueue_task(TaskControlBlock& tcb) noexcept
   {
      CORTOS_ASSERT_OP(tcb.effective_priority, <, config::MAX_PRIORITIES);
      CORTOS_ASSERT_OP(tcb.state, ==, TaskControlBlock::State::Ready); // Can only enqueue Ready tasks!
      matrix[tcb.effective_priority].push_back(tcb);
      bitmap |= (1u << tcb.effective_priority);
   }

   TaskControlBlock* pop_best_task() noexcept
   {
      if (bitmap == 0) return nullptr;
      auto const priority = std::countr_zero(bitmap);
      TaskControlBlock* tcb = matrix[priority].pop_front();
      if (matrix[priority].empty()) bitmap &= ~(1u << priority);
      return tcb;
   }

   void remove_task(TaskControlBlock& tcb) noexcept
   {
      auto const priority = tcb.effective_priority;
      matrix[priority].remove(tcb);
      if (matrix[priority].empty()) bitmap &= ~(1u << priority);
   }
};

} // namespace cortos

#endif // CORTOS_THREADING_SUBSYSTEM_HPP
