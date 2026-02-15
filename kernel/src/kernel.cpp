// Add doxygen comment here

#include "cortos/kernel.hpp"
#include "cortos/config.hpp"
#include "cortos/port.h"
#include "cortos/port_traits.h"
#include "mpsc_ring_buffer.hpp"

#include <bit>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <limits>

// Invariants list:
// Only core X mutates Scheduler[X].ready_matrix, current_task, blocked lists, etc.
// Cross-core ops must go through Scheduler::post_to_inbox() + cortos_port_send_reschedule_ipi(core).
// Spinlocks are only used with preemption disabled (and maybe IRQs) inside kernel land.


namespace cortos
{

/* ============================================================================
 * Configuration validation
 * ========================================================================= */
static_assert(1 <= config::CORES && config::CORES <= CORTOS_PORT_CORE_COUNT,
              "Port does not support configured amount of cores.");
static_assert(config::TIME_CORE_ID < config::CORES,
              "Time core set to non-existent core.");
static_assert(config::MAX_PRIORITIES < std::numeric_limits<uint32_t>::digits,
              "Priorities unsupported by kernel implementation.");

static constexpr std::uintptr_t align_down(std::uintptr_t v, std::size_t a) { return v & ~(static_cast<std::uintptr_t>(a) - 1); }
static constexpr std::uintptr_t   align_up(std::uintptr_t v, std::size_t a) { return (v + (a - 1)) & ~(static_cast<std::uintptr_t>(a) - 1); }

struct TaskControlBlock;
struct WaitGroup;


/**
 * @brief Intrusive wait-queue node for a thread waiting on a Waitable
 *
 * A WaitNode represents a single thread's participation in a wait operation
 * on a specific Waitable. Nodes are allocated from a per-thread pool and are
 * linked into a Waitable's wait queue for the duration of the wait.
 *
 * Each wait_for_any() call creates one WaitNode per Waitable involved.
 * Nodes are removed and returned to the pool when the wait completes.
 */
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
      waitable = nullptr;
      group    = nullptr;
      index    = INVALID_INDEX;
      // tcb and slot are explicitly not reset
   }

   [[nodiscard]] constexpr bool is_enqueued() const noexcept
   {
      return next != nullptr || prev != nullptr;
   }

   /**
    * @brief Attempt to wake the owning thread for this wait node
    *
    * Resolves a wait_for_any() race by attempting to "win" the associated WaitGroup.
    * If this node wins, all wait nodes participating in the same WaitGroup are
    * torn down (unlinked from their Waitables and returned to the per-thread pool),
    * and the owning thread is made runnable on its pinned core.
    *
    * If the WaitGroup has already been won by another node, this call is a no-op.
    *
    * @param acquired True if the woken thread acquired a resource as part of the wake
    *                 (e.g., mutex handoff). This is recorded in the WaitGroup result.
    */
   void wake_thread(bool acquired) const noexcept;
};

/**
 * @brief Per-thread pool of WaitNode objects
 *
 * Each thread owns a fixed-capacity pool of WaitNodes used to represent
 * active wait operations. This avoids dynamic allocation in the kernel
 * and guarantees bounded resource usage.
 *
 * A single wait_for_any() operation may allocate multiple nodes from the
 * pool (one per Waitable). All nodes are reclaimed when the wait completes.
 */
class WaitNodePool
{
   static constexpr std::size_t N = config::MAX_WAIT_NODES;
   static constexpr uint32_t ALL_NODES_FREE = (N == 32) ? std::numeric_limits<uint32_t>::max() : (1u << static_cast<uint32_t>(N)) - 1u;
   std::array<WaitNode, N> nodes{};
   uint32_t free_mask{ALL_NODES_FREE};

public:
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
      CORTOS_ASSERT(index != WaitNode::INVALID_INDEX);

      if (free_mask == 0) return nullptr;

      uint32_t bit = std::countr_zero(free_mask);
      free_mask &= ~(1u << bit);

      auto& node = nodes[bit];

      // Node should be inactive if the mask said it was free.
      // If not, we have a bug in free()/mask management.
      CORTOS_ASSERT(!node.active);

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

      CORTOS_ASSERT1(0 <= index && static_cast<std::size_t>(index) < N, index); // Node not from this pool

      auto& n = nodes[static_cast<std::size_t>(index)];
      CORTOS_ASSERT(n.active); // If error: You are freeing an inactive node.

      CORTOS_ASSERT_OP(n.slot, ==, static_cast<uint8_t>(index));
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
};

/**
 * @brief Coordination state for a multi-wait (wait_for_any) operation
 *
 * A WaitGroup tracks the outcome of a wait_for_any() call across multiple
 * Waitables. It records which waitable won the race and whether the waking
 * thread acquired a resource.
 *
 * Exactly one signal may win the group. All other signals are ignored once
 * the group is marked complete.
 */
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


static void thread_launcher(void* tcb_ptr);

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

   // Core pinning
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

   Waitable::Result block_on(std::span<Waitable* const> waitables)
   {
      CORTOS_ASSERT(state == State::Running);
      CORTOS_ASSERT(waitables.size() > 0);
      CORTOS_ASSERT(waitables.size() <= config::MAX_WAIT_NODES);

      // Snapshot once: all blocks are given the same snapshot. This disregards
      // all side-effects on_thread_blocked invocations have on the effective priority.
      auto const waiter = create_waiter();

      wait_group.begin(waitables.size());

      // Allocate and enqueue nodes
      for (std::size_t i = 0; auto* waitable : waitables) {
         CORTOS_ASSERT(waitable != nullptr);

         WaitNode* wait_node = wait_nodes.alloc(wait_group, *waitable, i++);
         CORTOS_ASSERT(wait_node != nullptr);

         waitable->on_thread_blocked(waiter);
         waitable->add(*wait_node);
      }

      state = State::Blocked;
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

      wait_nodes.for_each_active([&](WaitNode& node) {
         if (node.group != &group) return;

         // node.waitable is reset on remove, so capture it first to call the hook afterwards
         if (auto* waitable = node.waitable) {
            waitable->remove(node);
            waitable->on_thread_removed(waiter);
         }
         wait_nodes.free(node);
      }, &group);
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
   TaskControlBlock* head{nullptr};
   TaskControlBlock* tail{nullptr};

public:
   [[nodiscard]] constexpr bool              empty() const noexcept { return !head; }
   [[nodiscard]] constexpr bool           has_peer() const noexcept { return head && head != tail; }
   [[nodiscard]] constexpr TaskControlBlock* front() const noexcept { return head; }
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
   static constexpr std::size_t BITMAP_BITS = std::numeric_limits<uint32_t>::digits;
   std::array<TaskReadyQueue, config::MAX_PRIORITIES> matrix{};
   uint32_t bitmap{0};
   static_assert(config::MAX_PRIORITIES <= BITMAP_BITS, "bitmap cannot hold that many priorities!");

public:
   [[nodiscard]] constexpr int  best_priority()                    const noexcept { return bitmap ? std::countr_zero(bitmap) : -1; }
   [[nodiscard]] constexpr bool empty()                            const noexcept { return bitmap == 0; }
   [[nodiscard]] constexpr bool empty_at(uint32_t priority)        const noexcept { return matrix[priority].empty(); }
   [[nodiscard]] constexpr bool has_peer(uint32_t priority)        const noexcept { return matrix[priority].has_peer(); }
   [[nodiscard]] constexpr std::size_t size_at(uint32_t priority)  const noexcept { return matrix[priority].size(); }
   [[nodiscard]] constexpr std::bitset<BITMAP_BITS>  bitmap_view() const noexcept { return {bitmap}; }

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

static void idle_thread();

struct CrossCoreRequest
{
   enum : uint8_t {
      SetTaskReady, // Enqueue a TCB into this core's ready queue
   } type{};
   TaskControlBlock* tcb{nullptr};
};

class Scheduler
{
   std::uint32_t const core_id;
   std::atomic<uint32_t> pinned_task_counter{0};
   TaskControlBlock* current_task{nullptr};
   TaskControlBlock*    idle_task{nullptr};
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 4 * 1024> idle_stack{};

   TaskReadyMatrix ready_matrix;

   uint32_t    preempt_disable_depth{0};

   std::atomic<bool> inbox_poke_pending{false};
   static constexpr uint32_t INBOX_CAP = 64; // tune later
   MpscRingBuffer<CrossCoreRequest, INBOX_CAP> inbox;

public:
   static constexpr uint32_t IDLE_TASK_ID = 0; // Reserved

   constexpr explicit Scheduler(std::uint32_t core_id) : core_id(core_id) {};

   [[nodiscard]] uint32_t current_task_id()       const noexcept { return current_task ? current_task->id : 0; }
   [[nodiscard]] uint8_t  current_task_priority() const noexcept { return current_task ? current_task->effective_priority : 0; }
   [[nodiscard]] uint32_t pinned_task_count()     const noexcept { return pinned_task_counter.load(std::memory_order_relaxed); }

   void pin_task(TaskControlBlock& tcb)
   {
      tcb.pinned_core = core_id;
      pinned_task_counter.fetch_add(1, std::memory_order_relaxed);
   }

   void init_idle_task()
   {
      StackLayout slayout(idle_stack, 0);
      idle_task = ::new (slayout.tcb) TaskControlBlock(
         IDLE_TASK_ID,
         config::MAX_PRIORITIES-1,
         CoreAffinity::from_id(core_id),
         slayout.user_stack,
         idle_thread
      );
      idle_task->pinned_core = core_id;
   }

   // Core-local operations (only called on owning core)
   void start() noexcept
   {
      CORTOS_ASSERT(idle_task != nullptr); // init_idle_task() must run before start()

      auto* first = ready_matrix.pop_best_task();
      if (first == nullptr) {
         first = idle_task;
      }
      CORTOS_ASSERT(first != nullptr);
      CORTOS_ASSERT_OP(first->state, ==, TaskControlBlock::State::Ready);
      first->state = TaskControlBlock::State::Running;
      current_task = first;

      //cortos_port_set_thread_pointer(current_task);
      cortos_port_start_first(current_task->context());
   }

   void set_task_ready(TaskControlBlock& tcb) noexcept
   {
      CORTOS_ASSERT_OP(tcb.pinned_core, ==, core_id);

      tcb.state = TaskControlBlock::State::Ready;

      // Idle task does not belong in the ready_matrix,
      // but DOES follow state transition semantics
      if (&tcb == idle_task) return;

      ready_matrix.enqueue_task(tcb);
   }

   void drain_inbox() noexcept
   {
      inbox_poke_pending.store(false, std::memory_order_release);

      CrossCoreRequest request;
      while (inbox.pop(request)) {
         switch (request.type) {
            case CrossCoreRequest::SetTaskReady:
               set_task_ready(*request.tcb);
               break;
         }
      }
   }

   // Cross-core safe posting API
   bool post_to_inbox(CrossCoreRequest request) noexcept
   {
      // Many-producer safe
      if (!inbox.push(request)) return false; // Full

      bool expected = false;
      if (inbox_poke_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
         cortos_port_send_reschedule_ipi(core_id);
      }
      return true;
   }

   /**
   * @brief Selects the next runnable task for this core and performs a context switch.
   *
   * Invariants / contract:
   * - Called only by the owning core of this Scheduler (no cross-core mutation).
   * - @c current_task is non-null and is the task currently executing on this core.
   * - On entry, @c current_task->state is NEVER Ready:
   *     - Running    => treated as preempted/rotated and re-enqueued as Ready (except idle).
   *     - Blocked    => must already be removed from ready structures - not re-enqueued.
   *     - Terminated => must not be re-enqueued.
   * - The currently running task is not present in the ready matrix on entry.
   * - Any cross-core readying requests must be visible via @c drain_inbox() before selection.
   */
   void reschedule() noexcept
   {
      CORTOS_ASSERT(current_task);
      CORTOS_ASSERT(!current_task->is_enqueued());
      CORTOS_ASSERT(current_task->state != TaskControlBlock::State::Ready);

      drain_inbox();

      auto* previous_task = current_task;

      switch (previous_task->state) {
         case TaskControlBlock::State::Running:
            set_task_ready(*previous_task);
            break;

         case TaskControlBlock::State::Blocked:
         case TaskControlBlock::State::Terminated:
            break;

         case TaskControlBlock::State::Ready:
            __builtin_unreachable();
      }

      auto* next_task = ready_matrix.pop_best_task();
      if (!next_task) next_task = idle_task;

      current_task = next_task;
      next_task->state = TaskControlBlock::State::Running;
      cortos_port_switch(previous_task->context(), next_task->context());
   }

   Waitable::Result block_current_task(std::span<Waitable* const> waitables)
   {
      return current_task->block_on(waitables);
   }

   void disable_preemption()
   {
      ++preempt_disable_depth;
   }

   void enable_preemption()
   {
      CORTOS_ASSERT(preempt_disable_depth > 0);
      if (--preempt_disable_depth == 0) {
         cortos_port_pend_reschedule(); // TODO: Should this really be here?
      }
   }

   void reset()
   {
      CORTOS_ASSERT_OP(inbox.approx_size(), ==, 0); // Cannot reset whilst inbox is not empty
      CORTOS_ASSERT(ready_matrix.empty()); // Cannot reset whilst tasks still in the queue

      pinned_task_counter.store(0, std::memory_order_relaxed);
      inbox_poke_pending.store(false, std::memory_order_relaxed);
      preempt_disable_depth = 0;
      current_task = nullptr;
   }
};

static void reschedule_this_core();
static void core_entry();

// Note: this is a template purely because it allows the compile-time index-sequence array initialiser for schedulers to work
template<std::size_t CORES>
class Kernel
{
   Spinlock lock;
   std::array<Scheduler, CORES> schedulers;
   std::atomic<bool>    initialised{false};
   std::atomic<bool>        running{false};
   std::atomic<uint32_t> active_threads{0};
   std::atomic<uint32_t> thread_id_generator{1};

public:
   constexpr Kernel() noexcept : Kernel(std::make_index_sequence<CORES>{}) {}

   [[nodiscard]] bool          is_running()         const noexcept { return running.load(std::memory_order_acquire); }
   [[nodiscard]] std::uint32_t get_active_threads() const noexcept { return active_threads.load(std::memory_order_seq_cst); }

   [[nodiscard]] Scheduler& scheduler_for_core(std::uint32_t core_id) noexcept { return schedulers.at(core_id); }
   [[nodiscard]] Scheduler& scheduler_for_this_core()                 noexcept { return scheduler_for_core(cortos_port_get_core_id()); }
   [[nodiscard]] Scheduler& scheduler_for_time_core()                 noexcept { return scheduler_for_core(config::TIME_CORE_ID); }

   [[nodiscard]] std::uint32_t generate_thread_id() { return thread_id_generator.fetch_add(1, std::memory_order_relaxed); }

   void initialise() noexcept
   {
      CORTOS_ASSERT(!initialised); // Cannot invoke kernel::initialise twice (without finalising down in between)

      cortos_port_init(reschedule_this_core);

      for (auto& scheduler : schedulers) {
         scheduler.init_idle_task();
      }
      initialised = true;
   }

   void start() noexcept
   {
      CORTOS_ASSERT(initialised); // kernel::initialise() must be called first
      CORTOS_ASSERT(active_threads > 0); // Starting the kernel with no registered threads is not allowed

      running.store(true, std::memory_order_release);

      cortos_port_start_cores(config::CORES, core_entry);
   }

   void finalise() noexcept
   {
      CORTOS_ASSERT(initialised.load(std::memory_order_relaxed)); // In theory there shouldn't be anything to reset yet... so why was this invoked? smells buggy

      running.store(false, std::memory_order_relaxed);
      thread_id_generator.store(1, std::memory_order_relaxed);
      active_threads.store(0, std::memory_order_relaxed);;
      initialised.store(false, std::memory_order_relaxed);

      for (auto& scheduler : schedulers) {
         scheduler.reset();
      }
   }

   // Load-balancing pinner
   void pin_thread_to_core(TaskControlBlock& tcb) noexcept
   {
      uint32_t best_core = std::numeric_limits<uint32_t>::max();
      uint32_t best_load = std::numeric_limits<uint32_t>::max();

      bool found = false;
      for (uint32_t core = 0; core < schedulers.size(); ++core) {
         if (!tcb.affinity.allows(core)) continue;

         uint32_t load = schedulers[core].pinned_task_count();
         if (load < best_load) {
            best_load = load;
            best_core = core;
            found = true;
         }
      }
      CORTOS_ASSERT(found); // Thread affinity mask allows no cores

      schedulers.at(best_core).pin_task(tcb);
   }

   void register_thread(TaskControlBlock& tcb) noexcept
   {
      active_threads.fetch_add(1, std::memory_order_seq_cst);

      {
         SpinlockGuard guard(lock);
         pin_thread_to_core(tcb);
      }

      set_thread_ready(tcb);
   }

   void unregister_thread() noexcept
   {
      CORTOS_ASSERT(active_threads.load(std::memory_order_relaxed) != 0);
      active_threads.fetch_sub(1, std::memory_order_seq_cst);
   }

   /**
   * @brief Make a thread runnable on its pinned core (SMP-safe)
   *
   * Transitions @p tcb to the Ready state and enqueues it onto the ready queue of
   * its pinned core, respecting per-core scheduler ownership.
   */
   void set_thread_ready(TaskControlBlock& tcb) noexcept
   {
      CORTOS_ASSERT_OP(tcb.state, !=, TaskControlBlock::State::Terminated);

      auto& scheduler = scheduler_for_core(tcb.pinned_core);

      // If cores are not running yet, enqueue directly (even for remote cores)
      if (!running.load(std::memory_order_acquire)) {
         scheduler.set_task_ready(tcb);
         return;
      }

      auto this_core = cortos_port_get_core_id();
      if (this_core == tcb.pinned_core) {
         scheduler.set_task_ready(tcb);

         if (tcb.is_higher_priority_than(scheduler.current_task_priority())) {
            cortos_port_pend_reschedule();
         }
      } else {
         bool posted = scheduler.post_to_inbox({
            .type = CrossCoreRequest::SetTaskReady,
            .tcb = &tcb
         });
         CORTOS_ASSERT(posted);
      }
   }

private:
   template<std::size_t... Is>
   constexpr explicit Kernel(std::index_sequence<Is...>) noexcept : schedulers{ Scheduler{Is}... } {}
};
static constinit Kernel<config::CORES> KERNEL;

[[maybe_unused]] constexpr auto STATIC_SIZEOF_KERNEL = sizeof(KERNEL);

/**** KERNEL-global dependants ****/

// Registered as ISR handler for preemptive scheduling
static void reschedule_this_core()
{
   auto& scheduler = KERNEL.scheduler_for_this_core();

   scheduler.reschedule();
}

static void core_entry()
{
   auto& scheduler = KERNEL.scheduler_for_this_core();

   scheduler.start();
}

static void thread_launcher(void* tcb_ptr)
{
   auto* tcb = static_cast<TaskControlBlock*>(tcb_ptr);

   cortos_port_set_tls_pointer(tcb); // For now point TLS to the TCB, but in future, TLS sits just after TCB

   tcb->entry();

   tcb->state = TaskControlBlock::State::Terminated;
   // Signal joiners

   if (tcb->id == Scheduler::IDLE_TASK_ID) return; // Idle tasks are not apart of the same bookkeeping

   KERNEL.unregister_thread();
   cortos_port_thread_exit();
}

static void idle_thread()
{
   while (KERNEL.is_running()) {
      cortos_port_idle();
      cortos_port_pend_reschedule();
   }
}

void WaitNode::wake_thread(bool acquired) const noexcept
{
   CORTOS_ASSERT(tcb != nullptr);
   CORTOS_ASSERT(group != nullptr);
   CORTOS_ASSERT(waitable != nullptr);
   CORTOS_ASSERT(index != INVALID_INDEX);
   CORTOS_ASSERT_OP(tcb->state, ==, TaskControlBlock::State::Blocked);

   if (!group->try_win(static_cast<int>(index), acquired)) {
      return; // lost
   }

   // Winner: remove all nodes in this group (including this one)
   tcb->teardown_wait_group(*group);

   // Thread is ready to be enqueued on its pinned core
   KERNEL.set_thread_ready(*tcb);
}

/**** PUBLIC ****/

Thread::Thread(EntryFn&& entry, std::span<std::byte> stack, Priority priority, CoreAffinity affinity)
{
   StackLayout slayout(stack, 0);
   tcb = ::new (slayout.tcb) TaskControlBlock(
      KERNEL.generate_thread_id(),
      priority,
      affinity,
      slayout.user_stack,
      std::move(entry)
   );

   KERNEL.register_thread(*tcb);
}

Thread::~Thread()
{
   if (tcb == nullptr) return; // Thread handle has been moved from, or is otherwise empty
   CORTOS_ASSERT(tcb->state == TaskControlBlock::State::Terminated);
}

Thread::Thread(Thread&& other) noexcept : tcb(other.tcb)
{
   other.tcb = nullptr;
}

Thread& Thread::operator=(Thread&& other) noexcept
{
   tcb = other.tcb;
   other.tcb = nullptr;
   return *this;
}

[[nodiscard]] bool Waitable::empty() const noexcept
{
   return head == nullptr && tail == nullptr;
}

void Waitable::add(WaitNode& wait_node) noexcept
{
   CORTOS_ASSERT1(wait_node.waitable == this || wait_node.waitable == nullptr, wait_node.waitable);
   CORTOS_ASSERT(!wait_node.is_enqueued());

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
   uint8_t best_priority = std::numeric_limits<uint8_t>::max();

   for (auto* iter = head; iter; iter = iter->next) {
      CORTOS_ASSERT(iter->active);
      CORTOS_ASSERT(iter->tcb != nullptr);
      CORTOS_ASSERT(iter->waitable == this);

      // Skip nodes from already-satisfied wait_for_any groups (race window during teardown / multi-signal).
      if (iter->group && iter->group->done.load(std::memory_order_acquire)) continue;

      auto* tcb = iter->tcb;
      if (tcb->is_higher_priority_than(best_priority)) {
         best = iter;
         best_priority = tcb->effective_priority;
      }
   }
   return best;
}

void Waitable::signal_one(bool acquired) noexcept
{
   if (auto* wait_node = pick_best()) {
      wait_node->wake_thread(acquired);
   }
}

void Waitable::signal_all(bool acquired) noexcept
{
   while (true) {
      auto* wait_node = pick_best();
      if (!wait_node) break;
      wait_node->wake_thread(acquired);
   }
}

void Waitable::for_each_waiter(WaiterVisitor visitor) const
{
   // TODO: Might need a spinlock for cross-core waiting?
   for (auto* iter = head; iter; iter = iter->next) {
      if (!iter->active) continue;
      if (iter->group && iter->group->done.load(std::memory_order_acquire)) continue;
      if (!iter->tcb) continue;

      visitor(iter->tcb->create_waiter());
   }
}

void Spinlock::lock()
{
   KERNEL.scheduler_for_this_core().disable_preemption();
   while (flag.test_and_set(std::memory_order_acquire)) {
      // Busy-wait with CPU yield hint
      cortos_port_cpu_relax();
   }
}

void Spinlock::unlock()
{
   flag.clear(std::memory_order_release);
   KERNEL.scheduler_for_this_core().enable_preemption();
}

namespace kernel
{
   void initialise()
   {
      KERNEL.initialise();
   }

   void start()
   {
      KERNEL.start();
   }

   void finalise()
   {
      KERNEL.finalise();
   }

   std::uint32_t core_count() noexcept
   {
      return config::CORES;
   }

   std::uint32_t active_threads() noexcept
   {
      return KERNEL.get_active_threads();
   }

   Waitable::Result wait_for_any(std::span<Waitable* const> waitables)
   {
      CORTOS_ASSERT(waitables.size() > 0);
      CORTOS_ASSERT_OP(waitables.size(), <=, config::MAX_WAIT_NODES);

      auto& scheduler = KERNEL.scheduler_for_this_core();

      return scheduler.block_current_task(waitables);
   }

} // namespace kernel

namespace this_thread
{
   [[nodiscard]] Thread::Id id()
   {
      return KERNEL.scheduler_for_this_core().current_task_id();
   }

   [[nodiscard]] Thread::Priority priority()
   {
      return KERNEL.scheduler_for_this_core().current_task_priority();
   }

   [[nodiscard]] std::uint32_t core_id() noexcept
   {
      return cortos_port_get_core_id();
   }

   [[noreturn]] void thread_exit()
   {
      // TODO
      __builtin_unreachable();
   }

   void yield()
   {
      cortos_port_pend_reschedule();
   }
}  // namespace this_thread

}  // namespace cortos
