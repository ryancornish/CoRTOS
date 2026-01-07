// Add doxygen comment here

#include "cortos/kernel.hpp"
#include "cortos/port.h"

#include <bit>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <limits>

namespace cortos
{

static constexpr std::uintptr_t align_down(std::uintptr_t v, std::size_t a) { return v & ~(static_cast<std::uintptr_t>(a) - 1); }
static constexpr std::uintptr_t   align_up(std::uintptr_t v, std::size_t a) { return (v + (a - 1)) & ~(static_cast<std::uintptr_t>(a) - 1); }

struct TaskControlBlock;
struct WaitGroup;

struct WaitNode
{
   static constexpr uint8_t INVALID_INDEX = std::numeric_limits<uint8_t>::max();
   // Pool bookkeeping
   uint8_t slot{INVALID_INDEX};
   bool  active{false};

   // Intrusive links for the Waitable's waiter queue
   WaitNode* next{nullptr};
   WaitNode* prev{nullptr};

   TaskControlBlock* tcb{nullptr};
   Waitable*    waitable{nullptr};
   WaitGroup*      group{nullptr};

   // Which index in wait_for_any({span}) this node corresponds to
   uint8_t index{INVALID_INDEX};

   constexpr void reset() noexcept
   {
      active   = false;
      next = prev = nullptr;
      tcb      = nullptr;
      waitable = nullptr;
      group    = nullptr;
      index    = INVALID_INDEX;
      // tcb and slot are explicitly not reset
   }
};

class WaitNodePool
{
public:
   static constexpr std::size_t N = config::MAX_WAIT_NODES;
   static_assert(N > 0, "MAX_WAIT_NODES must be > 0");
   static_assert(N <= std::numeric_limits<uint32_t>::digits, "WaitNodePool currently supports up to 32 nodes via uint32_t mask");

   constexpr explicit WaitNodePool(TaskControlBlock* tcb) noexcept
   {
      for (std::size_t i = 0; auto& node : nodes) {
         node.tcb  = tcb;
         node.slot = i++;
      }
   }
   ~WaitNodePool() = default;

   WaitNodePool(WaitNodePool const&)            = delete;
   WaitNodePool& operator=(WaitNodePool const&) = delete;
   WaitNodePool(WaitNodePool&&)            = delete;
   WaitNodePool& operator=(WaitNodePool&&) = delete;

   void reset_all() noexcept
   {
      for (auto& node : nodes) {
         node.reset();
      }
      free_mask = ALL_NODES_FREE;
   }

   [[nodiscard]] constexpr std::size_t capacity()   const noexcept { return nodes.size(); }
   [[nodiscard]] constexpr std::size_t free_count() const noexcept { return std::popcount(free_mask); }
   [[nodiscard]] constexpr        bool empty()      const noexcept { return free_mask == 0; }

   /**
    * Allocate a node, initialize its identity fields, and return it.
    * Returns nullptr if pool exhausted.
    */
   WaitNode* alloc(WaitGroup& group, Waitable& waitable, uint8_t index) noexcept
   {
      assert(index != WaitNode::INVALID_INDEX);

      if (free_mask == 0) return nullptr;

      uint32_t bit = std::countr_zero(free_mask);
      free_mask &= ~(1u << bit);

      auto& node = nodes[bit];

      // Node should be inactive if the mask said it was free.
      // If not, we have a bug in free()/mask management.
      assert(node.active == false);

      node.reset();
      node.active   = true;
      node.group    = &group;
      node.waitable = &waitable;
      node.index    = index;

      return &node;
   }

   /**
    * Free a node back to the pool.
    * Caller is responsible for unlinking it from any Waitable queue first.
    */
   void free(WaitNode& node) noexcept
   {
      std::ptrdiff_t index = &node - nodes.data();
      // Ensure pointer belongs to this pool.
      if (index < 0 || static_cast<std::size_t>(index) >= N) {
         assert(false && "WaitNodePool::free: node not from this pool");
      }

      auto& n = nodes[static_cast<std::size_t>(index)];
      assert(n.active && "Freeing an inactive node");

      assert(n.slot == static_cast<uint8_t>(index));
      n.reset();
      free_mask |= (1u << static_cast<uint32_t>(index));
   }

   /**
    * Iterate active nodes (optionally filtered by group).
    * Useful for teardown: remove all nodes for a completed wait.
    */
   void for_each_active(Function<void(WaitNode&), 32, HeapPolicy::NoHeap>&& fn, WaitGroup* only_group = nullptr) noexcept
   {
      for (std::size_t i = 0; i < N; ++i) {
         auto& node = nodes[i];
         if (!node.active) continue;
         if (only_group && node.group != only_group) continue;
         fn(node);
      }
   }

   /**
    * Return pointer to node by slot index (even if inactive).
    * Primarily for debugging / assertions.
    */
   [[nodiscard]] WaitNode* at(std::size_t slot) noexcept
   {
      if (slot >= N) return nullptr;
      return &nodes[slot];
   }

private:
   static constexpr uint32_t ALL_NODES_FREE = (N == 32) ? std::numeric_limits<uint32_t>::max() : (1u << static_cast<uint32_t>(N)) - 1u;
   std::array<WaitNode, N> nodes{};
   uint32_t free_mask{ALL_NODES_FREE};
};

struct WaitGroup
{
   std::atomic<bool> done{false};
   int       winner_index{-1};
   bool          acquired{false}; // Flag for 'resource waitables' (e.g. mutex)
   uint8_t expected_count{0};     // Purely for debug/sanity checking

   void begin(uint8_t n) noexcept
   {
      done.store(false, std::memory_order_relaxed);
      winner_index   = -1;
      acquired       = false;
      expected_count = n;
   }

   // Called by signal path. Returns true if *this* call won.
   bool try_win(int index, bool acquired_) noexcept
   {
      bool expected = false;
      if (!done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
         return false;
      }
      winner_index = index;
      acquired     = acquired_;
      return true;
   }
};


static void thread_launcher(void* vtcb);

struct TaskControlBlock
{
   enum class State : uint8_t { Ready, Running, Blocked, Terminated };
   State state{State::Ready};

   // Intrusive 'linked-list' links for a TaskReadyQueue
   TaskControlBlock* next{nullptr};
   TaskControlBlock* prev{nullptr};
   // Intrusive link for the SleepMinHeap

   uint32_t id;
   uint8_t base_priority;
   uint8_t effective_priority; // Can change dynamically

   // Core pinning (future, but you already have affinity)
   std::uint32_t pinned_core{0};
   CoreAffinity  affinity;

   std::span<std::byte> stack;
   Thread::EntryFn entry;

   WaitGroup wait_group{};
   WaitNodePool wait_nodes{this};

   // Linked-list of threads wanting to join with me
   TaskControlBlock* join_waiters_head{nullptr};

   // Opaque, in-place port context storage
   alignas(CORTOS_PORT_CONTEXT_ALIGN) std::array<std::byte, CORTOS_PORT_CONTEXT_SIZE> context_storage{};
   [[nodiscard]] constexpr auto*       context()       noexcept { return reinterpret_cast<cortos_port_context_t*      >(context_storage.data()); }
   [[nodiscard]] constexpr auto const* context() const noexcept { return reinterpret_cast<cortos_port_context_t const*>(context_storage.data()); }

   TaskControlBlock(uint32_t id, Thread::Priority priority, CoreAffinity affinity, std::span<std::byte> stack, Thread::EntryFn&& entry) :
      id(id), base_priority(priority), effective_priority(priority), affinity(affinity), stack(stack), entry(std::move(entry))
   {
      cortos_port_context_init(context(), stack.data(), stack.size(), thread_launcher, this);
   }

   Waitable::Result block_on(std::span<Waitable* const> waitables)
   {
      // Preconditions
      // assert(state == Running);
      // assert(ws.size() > 0);
      // assert(ws.size() <= config::MAX_WAIT_NODES);

      wait_group.begin(static_cast<uint8_t>(waitables.size()));

      // Allocate and enqueue nodes
      for (std::size_t i = 0; auto* waitable : waitables) {
         assert(waitable != nullptr);

         WaitNode* wait_node = wait_nodes.alloc(wait_group, *waitable, i);
         assert(wait_node != nullptr);

         waitable->add(*wait_node);
         // w->on_thread_blocked(this_thread??)
      }

      // Transition to Blocked, then schedule
      // state = Blocked;
      // scheduler_block_current_and_switch();

      // When we resume, winner info is in wait_group
      return Waitable::Result{
         .index    = wait_group.winner_index,
         .acquired = wait_group.acquired,
      };
   }

   void teardown_wait_group(WaitGroup& group) noexcept
   {
      wait_nodes.for_each_active([&](WaitNode& node) {
         if (node.group != &group) return;

         if (node.waitable) node.waitable->remove(node);
         wait_nodes.free(node);
      }, &group);
   }
};

static void thread_launcher(void* vtcb)
{
   auto* tcb = static_cast<TaskControlBlock*>(vtcb);
   // For now point TLS to the TCB, but in future, TLS sits just after TCB
   cortos_port_set_tls_pointer(tcb);
   tcb->entry();

   // Signal joiners

   cortos_port_thread_exit();
}

// TODO: Scheduler method? For now free standing is fine
static void wake_node(WaitNode const& wait_node, bool acquired) noexcept
{
   assert(wait_node.tcb != nullptr && wait_node.group != nullptr);

   if (!wait_node.group->try_win(static_cast<int>(wait_node.index), acquired)) {
      return; // lost
   }

   // Winner: remove all nodes in this group (including this one)
   wait_node.tcb->teardown_wait_group(*wait_node.group);

   // Mark tcb runnable, pend reschedule, etc.
   // tcb->state = TaskControlBlock::State::Ready;
   // scheduler_make_ready(tcb);
   cortos_port_pend_reschedule();
}

// Carves a user-provided buffer region into:
// +----------------------+ <-- buffer's end (high address)
// +   TaskControlBlock   + (Fixed size)
// +----------------------+
// + Thread-local storage + (Variable size)
// +----------------------+
// +     User's stack     +
// +----------------------+ <-- buffer's base (low address)
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

      assert(tls_base >= base && "Buffer too small for TLS+TCB");

      auto const tls_offset = static_cast<std::size_t>(tls_base - base);
      auto const tls_length = static_cast<std::size_t>(tls_top  - tls_base);
      tls_region = buffer.subspan(tls_offset, tls_length); // zero-length span if tls_bytes == 0

      // User stack: everything below TLS
      auto const stack_len = static_cast<std::size_t>(tls_base - base);
      assert(stack_len > 64 && "Buffer too small after carving TCB/TLS");

      user_stack = buffer.subspan(0, stack_len);
   }
};

static std::atomic<uint32_t> next_thread_id{1};
Thread::Thread(EntryFn&& entry, std::span<std::byte> stack, Priority priority, CoreAffinity affinity)
{
   auto id = next_thread_id.fetch_add(1, std::memory_order_relaxed);

   StackLayout slayout(stack, 0);
   tcb = ::new (slayout.tcb) TaskControlBlock(id, priority, affinity, slayout.user_stack, std::move(entry));

   //set_task_ready(tcb);
}


// Waitable stuff
void Waitable::on_thread_blocked(Thread*) {}
void Waitable::on_thread_removed(Thread*) {}

[[nodiscard]] bool Waitable::empty() const noexcept
{
   return head == nullptr && tail == nullptr;
}

void Waitable::add(WaitNode& wait_node) noexcept
{
   assert(wait_node.waitable == this || wait_node.waitable == nullptr);
   assert(wait_node.next == nullptr && wait_node.prev == nullptr);

   wait_node.waitable = this;

   wait_node.prev = tail;
   wait_node.next = nullptr;
   if (tail) {
      tail->next = &wait_node;
   } else {
      head = &wait_node;
   }
   tail = &wait_node;
}

void Waitable::remove(WaitNode& wait_node) noexcept
{
   if (wait_node.waitable != this) return;

   if (wait_node.prev) {
      wait_node.prev->next = wait_node.next;
   } else {
      head = wait_node.next;
   }
   if (wait_node.next) {
      wait_node.next->prev = wait_node.prev;
   } else {
      tail = wait_node.prev;
   }
   wait_node.prev = wait_node.next = nullptr;
   wait_node.waitable = nullptr;
}

WaitNode* Waitable::pick_best() noexcept
{
   WaitNode* best = nullptr;
   uint8_t best_priority = 0;

   for (auto* iter = head; iter; iter = iter->next) {
      if (!iter->active) continue;
      if (iter->group && iter->group->done.load(std::memory_order_acquire)) continue;

      auto* tcb = iter->tcb;
      if (!tcb) continue;

      auto priority = tcb->effective_priority;
      if (!best || priority > best_priority) {
         best = iter;
         best_priority = priority;
      }
   }
   return best;
}

void Waitable::signal_one(bool acquired) noexcept
{
   if (auto* wait_node = pick_best()) {
      wake_node(*wait_node, acquired);
   }
}

void Waitable::signal_all(bool acquired) noexcept
{
   while (true) {
      auto* wait_node = pick_best();
      if (!wait_node) break;
      wake_node(*wait_node, acquired);
   }
}


template<typename Fn>
void Waitable::for_each_waiter(Fn&& fn)
{
   (void)fn;
}

namespace kernel
{

   static bool g_initialised = false;

   void initialise()
   {
      if (g_initialised) return;
      g_initialised = true;

      // TODO: init scheduler structures per core (ready queues, current thread pointers, etc.)
      // TODO: init idle threads per core (or lazily)
      // TODO: set up port (cortos_port_init)
   }

   [[noreturn]] void start()
   {
      assert(g_initialised && "kernel::initialise() must be called first");

      // TODO: pick initial threads for each core, set TLS, start first context on each core
      // For now:
      while (true) {
         cortos_port_idle();
      }
   }

   std::uint32_t core_count() noexcept
   {
      return cortos_port_get_core_count();
   }

   Waitable::Result wait_for_any(std::span<Waitable* const> waitables)
   {
      assert(waitables.size() > 0);
      assert(waitables.size() <= config::MAX_WAIT_NODES);

      // auto* tcb = scheduler_current_tcb();
      // return tcb->block_on(waitables);

      return {}; // TODO
   }

} // namespace kernel


void Spinlock::lock()
{
   while (flag.test_and_set(std::memory_order_acquire)) {
      // Busy-wait with CPU yield hint
      cortos_port_cpu_relax();
   }
}

}  // namespace cortos
