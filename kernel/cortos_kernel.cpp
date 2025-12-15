/**
 *
 */
#include "cortos.hpp"
#include "port.h"
#include "port_traits.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <bitset>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <span>
#include <variant>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

namespace cortos
{
   static constexpr uint32_t UINT32_BITS = std::numeric_limits<uint32_t>::digits;
   static constexpr uint8_t IDLE_THREAD_ID = 1; // Reserved
   static constexpr Thread::Priority IDLE_PRIORITY(Config::MAX_PRIORITIES - 1);
   static constexpr Thread::Priority TIMER_PRIORITY(Config::TIMER_THREAD_PRIORITY);

   static constexpr std::size_t align_down(std::size_t size, std::size_t align) { return size & ~(align - 1); }
   static constexpr std::size_t align_up(std::size_t size, std::size_t align)   { return (size + (align - 1)) & ~(align - 1); }

   template<typename Traits>
   class IntrusiveMinHeap
   {
   public:
      using Node      = typename Traits::Node;
      using IndexType = uint16_t;

   private:
      static constexpr IndexType CAPACITY = Traits::CAPACITY;
      std::array<Node*, CAPACITY> heap_buffer{};
      IndexType size_count{0};

      static IndexType parent(IndexType i) noexcept { return (i - 1u) >> 1; }
      static IndexType left  (IndexType i) noexcept { return (i << 1) + 1u; }
      static IndexType right (IndexType i) noexcept { return (i << 1) + 2u; }

      static bool earlier(const Node* a, const Node* b) noexcept { return Traits::earlier(a, b); }

      static IndexType& index(Node* n) noexcept { return Traits::index(n); }

      void swap_nodes(IndexType a, IndexType b) noexcept
      {
         std::swap(heap_buffer[a], heap_buffer[b]);
         index(heap_buffer[a]) = a;
         index(heap_buffer[b]) = b;
      }

      void sift_up(IndexType i) noexcept
      {
         while (i > 0) {
            IndexType p = parent(i);
            if (!earlier(heap_buffer[i], heap_buffer[p])) break;
            swap_nodes(i, p);
            i = p;
         }
      }

      void sift_down(IndexType i) noexcept
      {
         while (true) {
            IndexType l = left(i), r = right(i), m = i;
            if (l < size_count && earlier(heap_buffer[l], heap_buffer[m])) m = l;
            if (r < size_count && earlier(heap_buffer[r], heap_buffer[m])) m = r;
            if (m == i) break;
            swap_nodes(i, m);
            i = m;
         }
      }

   public:
      [[nodiscard]] bool empty() const noexcept { return size_count == 0; }
      [[nodiscard]] IndexType size() const noexcept { return size_count; }
      [[nodiscard]] Node* top() const noexcept { return size_count ? heap_buffer[0] : nullptr; }

      void push(Node* n) noexcept
      {
         // You can assert if you want a hard fail on overflow:
         // assert(size_count < CAPACITY);
         IndexType i = size_count++;
         heap_buffer[i] = n;
         index(n) = i;
         sift_up(i);
      }

      Node* pop_min() noexcept
      {
         if (!size_count) return nullptr;
         Node* n = heap_buffer[0];
         index(n) = std::numeric_limits<IndexType>::max();
         --size_count;
         if (size_count) {
            heap_buffer[0] = heap_buffer[size_count];
            index(heap_buffer[0]) = 0;
            sift_down(0);
         }
         return n;
      }

      void remove(Node* n) noexcept
      {
         IndexType i = index(n);
         if (i == std::numeric_limits<IndexType>::max()) return; // not in heap

         index(n) = std::numeric_limits<IndexType>::max();
         --size_count;
         if (i == size_count) return; // removed last

         heap_buffer[i] = heap_buffer[size_count];
         index(heap_buffer[i]) = i;

         // Re-heapify from i (either direction)
         if (i > 0 && earlier(heap_buffer[i], heap_buffer[parent(i)])) {
            sift_up(i);
         } else {
            sift_down(i);
         }
      }
   };

   struct WaitTarget
   {
      std::variant<std::monostate, MutexImpl*, SemaphoreImpl*, ConditionVarImpl*> sync_prim;
      bool acquired{false};
      template<typename T> WaitTarget& operator=(T t) noexcept { sync_prim = t; acquired = false; return *this; }
      void clear() noexcept { sync_prim = std::monostate{}; acquired = false; }
      constexpr operator bool() const noexcept { return !std::holds_alternative<std::monostate>(sync_prim); }
      void remove(TaskControlBlock* tcb) noexcept;
   };

   template<typename T> struct LinkedListNode { T* next; T* prev; };

   // A WaitNode is the registration record that tracks 'this thread is waiting on this waitable'
   // A thread can wait on multiple waitables
   struct WaitNode
   {
      TaskControlBlock* tcb{nullptr};
      Waitable*         owner{nullptr};
      LinkedListNode<WaitNode> node{.next=nullptr,.prev=nullptr};
      uint16_t slot{}; // Index within tcb->wait.nodes[]
      bool     linked{false};
   };

   struct WaitState
   {
      // Result latched by whichever event wins
      std::atomic_bool triggered{false};
      int              winner{-1};      // Slot index that triggered
      bool             acquired{false}; // E.g. token granted, mutex ownership granted
   };

   // Intrusive doubly-linked-list of Task Control Blocks.
   // Used for:
   // - Task Round-Robin ready queues within the ReadyMatrix.
   // - Sync primitive waiter queues within Mutex/Semaphore/ConditionVar.
   // - Thread joining waiter queues within the TaskControlBlock itself.
   class TaskQueue
   {
      TaskControlBlock* head;
      TaskControlBlock* tail;

   public:
      [[nodiscard]] constexpr bool              empty() const noexcept { return !head; }
      [[nodiscard]] constexpr bool           has_peer() const noexcept { return head && head != tail; }
      [[nodiscard]] constexpr TaskControlBlock* front() const noexcept { return head; }

      // This walks the linked list so isn't 'free'. Will be used for debugging only for now
      [[nodiscard]] std::size_t size() const noexcept;
      void push_back(TaskControlBlock* tcb) noexcept;
      TaskControlBlock* pop_front() noexcept;
      void remove(TaskControlBlock* tcb) noexcept;
      // FIFO among tcb's with equal priority. Used by sync primitive wait lists, not RR queue
      TaskControlBlock* pop_highest_priority() noexcept;
   };
   static_assert(std::is_trivially_constructible_v<TaskQueue>, "Must be usable within OpaqueImpl structs");
   static_assert(std::is_standard_layout_v<TaskQueue>,         "Must be usable within OpaqueImpl structs");

   // Per-Thread metadata.
   // Is constructed-in-place within the user defined stack.
   // ^ See StackLayout for user stack layout details
   struct TaskControlBlock
   {
      enum class State : uint8_t { Ready, Running, Sleeping, Blocked, Terminated };
      State state{State::Ready};

      // Intrusive link for a TaskQueue
      // Note: Because there is only one link here, the following invariant _must_ hold at all times:
      // This TaskControlBlock object may only be a member of at-most ONE TaskQueue at any given moment!
      LinkedListNode<TaskControlBlock> tq_node{.next = nullptr, .prev = nullptr};
      // Intrusive link for the SleepMinHeap
      uint16_t sleep_index{UINT16_MAX}; // UINT16_MAX == not in the SleepMinHeap
      Tick wake_tick{0};

      bool cancel_requested{false};
      // User-defined properties
      uint32_t id;
      uint8_t const base_priority;
      uint8_t priority{base_priority}; // AKA effective-priority - can change dynamically
      std::span<std::byte> stack;
      Thread::Entry entry;

      WaitState wait_state{};
      std::array<WaitNode, Config::MAX_WAIT_OBJECTS> wait_nodes{};
      uint32_t wait_mask{0};

      // List of mutexes currently held by me (used for priority inheritance)
      MutexImpl* held_mutex_head{nullptr};
      // List of threads wanting to join with me
      TaskQueue join_waiters{};

      // Opaque, in-place port context storage
      OpaqueImpl<port_context_t, CORTOS_PORT_CONTEXT_SIZE, CORTOS_PORT_CONTEXT_ALIGN> context_storage;
      constexpr port_context_t*       context()       noexcept { return context_storage.get(); }
      constexpr port_context_t const* context() const noexcept { return context_storage.get(); }

      TaskControlBlock(uint32_t id, Thread::Priority priority, std::span<std::byte> stack, Thread::Entry&& entry) :
         id(id), base_priority(priority), stack(stack), entry(std::move(entry))
      {
         for (int i = 0; auto& wn : wait_nodes) {
            wn.tcb = this;
            wn.slot = i++;
         }
      }
   };

   // --- Start Section: TaskQueue implementation ---
   [[nodiscard]] std::size_t TaskQueue::size() const noexcept
   {
      std::size_t n = 0;
      for (auto* tcb = head; tcb; tcb = tcb->tq_node.next) ++n;
      return n;
   }

   void TaskQueue::push_back(TaskControlBlock* tcb) noexcept
   {
      assert(tcb->tq_node.next == nullptr && tcb->tq_node.prev == nullptr && "TCB already linked");
      tcb->tq_node.next = nullptr;
      tcb->tq_node.prev = tail;
      if (tail) tail->tq_node.next = tcb; else head = tcb;
      tail = tcb;
   }

   TaskControlBlock* TaskQueue::pop_front() noexcept
   {
      if (!head) return nullptr;
      auto* tcb = head;
      head = tcb->tq_node.next;
      if (head) head->tq_node.prev = nullptr; else tail = nullptr;
      tcb->tq_node.next = tcb->tq_node.prev = nullptr;
      return tcb;
   }

   void TaskQueue::remove(TaskControlBlock* tcb) noexcept
   {
      if (tcb->tq_node.prev) tcb->tq_node.prev->tq_node.next = tcb->tq_node.next; else head = tcb->tq_node.next;
      if (tcb->tq_node.next) tcb->tq_node.next->tq_node.prev = tcb->tq_node.prev; else tail = tcb->tq_node.prev;
      tcb->tq_node.next = tcb->tq_node.prev = nullptr;
   }

   TaskControlBlock* TaskQueue::pop_highest_priority() noexcept
   {
      if (!head) return nullptr;

      TaskControlBlock* best     = head;
      uint8_t           best_pri = head->priority;

      for (auto* tcb = head->tq_node.next; tcb; tcb = tcb->tq_node.next) {
         if (tcb->priority < best_pri) {
            best     = tcb;
            best_pri = tcb->priority;
         }
      }
      remove(best);
      return best;
   }
   // --- End Section: TaskQueue implementation ---

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

   class ReadyMatrix
   {
      using RoundRobinQueue = TaskQueue;
      std::array<RoundRobinQueue, Config::MAX_PRIORITIES> matrix{};
      uint32_t bitmap{0};
      static_assert(Config::MAX_PRIORITIES <= UINT32_BITS, "bitmap cannot hold that many priorities!");

   public:
      [[nodiscard]] constexpr bool empty() const noexcept { return bitmap == 0; }
      [[nodiscard]] constexpr bool empty_at(uint32_t priority) const noexcept
      {
         return matrix[priority].empty();
      }
      [[nodiscard]] std::size_t size_at(uint32_t priority) const noexcept
      {
         return matrix[priority].size();
      }
      [[nodiscard]] constexpr bool has_peer(uint32_t priority) const noexcept
      {
         return matrix[priority].has_peer();
      }
      [[nodiscard]] constexpr int best_priority() const noexcept
      {
         return bitmap ? std::countr_zero(bitmap) : -1;
      }
      [[nodiscard]] std::bitset<UINT32_BITS> bitmap_view() const noexcept
      {
         return {bitmap};
      }

      void enqueue_task(TaskControlBlock* tcb) noexcept
      {
         matrix[tcb->priority].push_back(tcb);
         bitmap |= (1u << tcb->priority);
      }

      TaskControlBlock* pop_highest_task() noexcept
      {
         if (bitmap == 0) return nullptr;
         auto const priority = std::countr_zero(bitmap);
         TaskControlBlock* tcb = matrix[priority].pop_front();
         if (matrix[priority].empty()) bitmap &= ~(1u << priority);
         return tcb;
      }

      [[nodiscard]] TaskControlBlock* peek_highest_task() const noexcept
      {
         if (bitmap == 0) return nullptr;
         auto const priority = std::countr_zero(bitmap);
         return matrix[priority].front();
      }

      void remove_task(TaskControlBlock* tcb) noexcept
      {
         auto const priority = tcb->priority;
         matrix[priority].remove(tcb);
         if (matrix[priority].empty()) bitmap &= ~(1u << priority);
      }
   };

   struct SleepHeapTraits
   {
      using Node = TaskControlBlock;
      static constexpr uint16_t CAPACITY = Config::MAX_THREADS;
      static uint16_t& index(Node* tcb) noexcept { return tcb->sleep_index; }
      static bool earlier(Node const* a, Node const* b) noexcept { return a->wake_tick.is_before(b->wake_tick); }
   };
   using SleepMinHeap = IntrusiveMinHeap<SleepHeapTraits>;

   struct TimerImpl
   {
      Timer::Callback cb;
      Timer::Mode mode;
      Timer* owner;
      bool armed{false};
      Tick deadline;
      uint16_t heap_index{std::numeric_limits<uint16_t>::max()};
      TimerImpl(Timer::Callback&& cb, Timer::Mode mode, Timer* owner)
         : cb(std::move(cb)), mode(mode), owner(owner) {}
      TimerImpl(TimerImpl&&) = default;
   };
   static_assert(sizeof(TimerImpl) == sizeof(Timer::ImplStorage),   "Adjust forward size declaration in header to match");
   static_assert(alignof(TimerImpl) == alignof(Timer::ImplStorage), "Adjust forward align declaration in header to match");

   struct TimerHeapTraits
   {
      using Node = TimerImpl;
      static constexpr uint16_t CAPACITY = Config::MAX_TIMERS;
      static uint16_t& index(Node* timer) noexcept { return timer->heap_index; }
      static bool earlier(Node const* a, Node const* b) noexcept { return a->deadline.is_before(b->deadline); }
   };
   using TimerMinHeap = IntrusiveMinHeap<TimerHeapTraits>;

   struct TimerJobQueue
   {
      std::array<Timer::Callback, TimerHeapTraits::CAPACITY> buffer{};
      std::size_t head{0};
      std::size_t tail{0};
      std::size_t count{0};
      [[nodiscard]] bool empty() const noexcept { return count == 0; }

      bool push(Timer::Callback&& job)
      {
         if (count == buffer.size()) {
            // This is a bug in the kernel config: timer service saturated.
            std::abort();
         }
         buffer[tail] = std::move(job);
         tail = (tail + 1) % buffer.size();
         ++count;
         return true;
      }

      std::optional<Timer::Callback> pop()
      {
         if (!count) return std::nullopt;
         auto cb = std::make_optional(std::move(buffer[head]));
         head = (head + 1) % buffer.size();
         --count;
         return cb;
      }
   };

   class AtomicDeadline
   {
      static constexpr uint32_t DISARMED = std::numeric_limits<uint32_t>::max();
      std::atomic_uint32_t deadline{DISARMED};

   public:
      [[nodiscard]] bool armed(std::memory_order mo = std::memory_order_relaxed) const noexcept
      {
         return deadline.load(mo) != DISARMED;
      }
      [[nodiscard]] bool due(Tick now, std::memory_order mo = std::memory_order_relaxed) const noexcept
      {
         uint32_t dline = deadline.load(mo);
         return dline != DISARMED && now.has_reached(dline);
      }
      void store(Tick tick, std::memory_order mo = std::memory_order_relaxed) noexcept
      {
         deadline.store(tick.value(), mo);
      }
      void disarm() noexcept
      {
         deadline.store(DISARMED, std::memory_order_relaxed);
      }
   };

   struct InternalSchedulerState
   {
      std::atomic_uint32_t next_thread_id{2}; // 0 is invalid, 1 is reserved for idle, 2..N are user threads
      std::atomic_bool     preempt_disabled{true};
      std::atomic_bool     need_reschedule{false};
      TaskControlBlock* current_task{nullptr};
      TaskControlBlock* timer_tcb{nullptr};
      TaskControlBlock* idle_tcb{nullptr};

      ReadyMatrix ready_matrix;
      AtomicDeadline next_slice_tick; // When the next RR rotation is due
      SleepMinHeap sleepers;
      AtomicDeadline next_wake_tick; // When next sleeper must be woken
      TimerMinHeap timers;
      AtomicDeadline next_expiry_tick; // When next timer expires
      TimerJobQueue timer_job_queue;
   };
   static constinit InternalSchedulerState iss;

   static void set_task_ready(TaskControlBlock* tcb)
   {
      LOG_SCHED("set_task_ready() @ID(%u) @PRIO(%u)", tcb->id, tcb->priority);

      tcb->state = TaskControlBlock::State::Ready;
      iss.ready_matrix.enqueue_task(tcb);
      // If the new task is higher priority than the running one, we need to reschedule.
      if (!iss.current_task || tcb->priority < iss.current_task->priority) {
         iss.need_reschedule.store(true, std::memory_order_relaxed);
      }
   }

   static void set_effective_priority(TaskControlBlock* tcb, uint8_t new_priority)
   {
      if (new_priority == tcb->priority) return;
      auto const old_priority = tcb->priority; // Just capturing for logging

      bool was_ready = tcb->state == TaskControlBlock::State::Ready;
      if (was_ready) iss.ready_matrix.remove_task(tcb);

      tcb->priority = new_priority;
      if (was_ready) iss.ready_matrix.enqueue_task(tcb);
      LOG_SCHED("set_effective_priority() @ID(%u) %u -> %u (state=%s, was_ready=%s)", tcb->id, old_priority, new_priority, STATE_TO_STR(tcb->state), TRUE_FALSE(was_ready));
      // Let the scheduler re-evaluate who should run
      iss.need_reschedule.store(true, std::memory_order_relaxed);
   }

   static void context_switch_to(TaskControlBlock* next)
   {
      // "do-not-switch-to-same-task" optimisation does not work
      // under simulation when the only runnable thread yields
      if constexpr (!CORTOS_SIMULATION) {
         if (next == iss.current_task) return;
      }
      TaskControlBlock* previous_task = iss.current_task;
      iss.current_task = next;
      iss.current_task->state = TaskControlBlock::State::Running;

      LOG_SCHED("context_switch_to() from @ID(%u) -> to @ID(%u)", previous_task ? previous_task->id : 0u, next->id);
      port_set_thread_pointer(next);
      port_switch(previous_task ? previous_task->context() : nullptr, iss.current_task->context());
      LOG_SCHED("~context_switch_to() returned to scheduler");
   }

   static void schedule()
   {
      if (iss.preempt_disabled.load(std::memory_order_acquire)) return;
      if (!iss.need_reschedule.exchange(false)) return;

      LOG_SCHED_READY_MATRIX();

      auto const now = Scheduler::tick_now();

      // Wake sleepers
      while (true) {
         auto* top_tcb = iss.sleepers.top();
         if (!top_tcb || now.is_before(top_tcb->wake_tick)) break; // Noone asleep or not due yet
         (void)iss.sleepers.pop_min();
         if (top_tcb->wait_target) top_tcb->wait_target.remove(top_tcb); // Sleeper was waiting for a sync prim
         LOG_SCHED("schedule() sleeper @ID(%u) woken up for @ALARM(%u)", top_tcb->id, top_tcb->wake_tick.value());
         set_task_ready(top_tcb);
      }

      // Update when the next sleeper will wake up
      if (auto* top_tcb = iss.sleepers.top()) {
         iss.next_wake_tick.store(top_tcb->wake_tick);
      } else {
         iss.next_wake_tick.disarm();
      }

      // Expire timers
      while (true) {
         auto* top_timer = iss.timers.top();
         if (!top_timer || now.is_before(top_timer->deadline)) break;
         (void)iss.timers.pop_min();
         top_timer->armed = false;
         if (top_timer->cb) {
            iss.timer_job_queue.push(std::move(top_timer->cb));
            LOG_SCHED("schedule() @TIMER(%u) expired on @ALARM(%u)", ptr_suffix(top_timer->owner), top_timer->deadline.value());
            if (iss.timer_tcb->state == TaskControlBlock::State::Blocked) set_task_ready(iss.timer_tcb);
         }
      }

      // Update when the next timer will expire
      if (auto* top_timer = iss.timers.top()) {
         iss.next_expiry_tick.store(top_timer->deadline);
      } else {
         iss.next_expiry_tick.disarm();
      }

      // Round robin rotation
      if (iss.next_slice_tick.due(now) &&
          iss.current_task &&
          iss.current_task != iss.idle_tcb &&
          iss.current_task->state == TaskControlBlock::State::Running &&
          iss.ready_matrix.has_peer(iss.current_task->priority)) // Don't requeue for singular threads at a priority level
      {
         LOG_SCHED("schedule() time-slice RR rotate @ID(%u) to the back of the queue", iss.current_task->id);
         set_task_ready(iss.current_task);
      }

      // Choose next runnable
      auto* next_task = iss.ready_matrix.pop_highest_task();
      if (!next_task) {
         iss.next_slice_tick.disarm();
         if (iss.current_task != iss.idle_tcb) {
            LOG_SCHED("schedule() pick IDLE @ID(%u) @PRIO(%u)", iss.idle_tcb->id, IDLE_PRIORITY);
            context_switch_to(iss.idle_tcb);
         }
         return;
      }

      // If we are preempted by a higher thread, push current back to queue
      if (iss.current_task &&
          iss.current_task != iss.idle_tcb && // Idle does not belong in the ready matrix
          iss.current_task != next_task &&
          iss.current_task->state == TaskControlBlock::State::Running)
      {
         set_task_ready(iss.current_task);
      }

      if (next_task->priority == IDLE_PRIORITY) {
         iss.next_slice_tick.disarm();
      } else {
         // Serve a fresh slice
         iss.next_slice_tick.store(now + Config::TIME_SLICE);
      }

      LOG_SCHED("schedule() pick @ID(%u) @PRIO(%u)", next_task->id, next_task->priority);

      context_switch_to(next_task);
   }

   static void thread_launcher(void* void_arg_is_tcb)
   {
      auto* tcb = static_cast<TaskControlBlock*>(void_arg_is_tcb);
      LOG_THREAD("thread_launcher() @ID(%u)", tcb->id);
      tcb->entry();
      {
         Scheduler::Lock guard;

         tcb->state = TaskControlBlock::State::Terminated;
         // Wake any joiners
         while (auto* waiter = tcb->join_waiters.pop_front()) {
            LOG_THREAD("signalling join waiter @ID(%u)", waiter->id);
            set_task_ready(waiter);
         }
         cortos_request_reschedule();
      }
      LOG_THREAD("@ID(%u) Terminated", tcb->id);
      port_thread_exit(); // Switch away and never come back
      if constexpr (!CORTOS_SIMULATION) __builtin_unreachable();
   }

   // --- Start Section: Kernel-owned threads ---
   alignas(CORTOS_STACK_ALIGN) static std::array<std::byte, 4096> timer_stack{}; // TODO: size stack
   static void timer_entry()
   {
      while (!iss.current_task->cancel_requested) {
         while (true) {
            Timer::Callback job;
            {
               Scheduler::Lock guard;

               auto opt_job = iss.timer_job_queue.pop();
               if (!opt_job) {
                  // No more jobs *as of now*. Park this thread until scheduler wakes it.
                  LOG_THREAD("timer_entry() marking self as blocked");
                  iss.current_task->state = TaskControlBlock::State::Blocked;
                  break;
               }
               job = std::move(*opt_job);
            }
            LOG_THREAD("timer_entry() executing timer cb");
            job();
         }
         Scheduler::yield();
      }
   }

   alignas(CORTOS_STACK_ALIGN) static std::array<std::byte, 4096> idle_stack{}; // TODO: size stack
   static void idle_entry()
   {
      while(!iss.current_task->cancel_requested) {
         port_idle();
      }
   }
   // --- End Section: Kernel-owned threads ---

   // --- Start Section: Scheduler public methods ---
   void Scheduler::init(uint32_t tick_hz)
   {
      LOG_SCHED("Scheduler::init(%u hz)", tick_hz);
      port_init(tick_hz);

      // Create the idle thread
      StackLayout idle_slayout(idle_stack, 0);
      iss.idle_tcb = ::new (idle_slayout.tcb) TaskControlBlock(
         IDLE_THREAD_ID,
         IDLE_PRIORITY,
         idle_slayout.user_stack,
         idle_entry
      );

      port_context_init(iss.idle_tcb->context(),
                        idle_slayout.user_stack.data(),
                        idle_slayout.user_stack.size(),
                        thread_launcher,
                        iss.idle_tcb);
      // Note: we intentionally do NOT call set_task_ready(idle_tcb).
      // Idle is never in ready_matrix.

      // Create the timer thread
      StackLayout timer_slayout(timer_stack, 0);
      auto id = iss.next_thread_id.fetch_add(1, std::memory_order_relaxed);
      iss.timer_tcb = ::new (timer_slayout.tcb) TaskControlBlock(
         id,
         TIMER_PRIORITY,
         timer_slayout.user_stack,
         timer_entry
      );

      port_context_init(iss.timer_tcb->context(),
                        timer_slayout.user_stack.data(),
                        timer_slayout.user_stack.size(),
                        thread_launcher,
                        iss.timer_tcb);

      set_task_ready(iss.timer_tcb);
   }

   void Scheduler::start()
   {
      LOG_SCHED("Scheduler::start()");
      LOG_SCHED_READY_MATRIX();

      auto* first = iss.ready_matrix.pop_highest_task();
      assert(first != nullptr);

      LOG_SCHED("Scheduler::start() pick first @ID(%u) @PRIO(%u)", first->id, first->priority);

      iss.current_task = first;
      iss.current_task->state = TaskControlBlock::State::Running;
      iss.next_slice_tick.store(Scheduler::tick_now() + Config::TIME_SLICE);
      iss.preempt_disabled.store(false, std::memory_order_release); // Open the scheduler

      port_set_thread_pointer(iss.current_task); // I think this is correct?
      port_start_first(iss.current_task->context());

      if constexpr (!CORTOS_SIMULATION) {
         __builtin_unreachable();
      }
   }

   void Scheduler::kill_timer_thread()
   {
      Scheduler::Lock guard;
      iss.timer_tcb->cancel_requested = true;
      iss.timer_job_queue.push([]{}); // Trigger the timer thread to run
   }

   void Scheduler::kill_idle_thread()
   {
      Scheduler::Lock guard;
      iss.idle_tcb->cancel_requested = true;
   }

   void Scheduler::yield()
   {
      LOG_SCHED("yield() by @ID(%u)", iss.current_task->id);

      if (iss.current_task && iss.current_task->state == TaskControlBlock::State::Running) {
         set_task_ready(iss.current_task); // put current at tail
      }
      cortos_request_reschedule();
      port_yield();
   }

   Tick Scheduler::tick_now()
   {
      return Tick(port_tick_now());
   }

   void Scheduler::sleep_for(uint32_t ticks)
   {
      if (ticks == 0) { yield(); return; }
      // Put current to sleep
      assert(iss.current_task && "no current thread");
      assert(iss.current_task->sleep_index == UINT16_MAX && "thread is already sleeping");
      iss.current_task->state = TaskControlBlock::State::Sleeping;
      iss.current_task->wake_tick = tick_now() + (ticks);

      LOG_SCHED("sleep_for() by @ID(%u) until @ALARM(%u) (+%u ticks)", iss.current_task->id, iss.current_task->wake_tick.value(), ticks);

      iss.sleepers.push(iss.current_task);

      if (auto const* top_tcb = iss.sleepers.top()) {
         iss.next_wake_tick.store(top_tcb->wake_tick);
      }

      cortos_request_reschedule();
      port_yield();
   }

   void Scheduler::preempt_disable() { port_preempt_disable(); iss.preempt_disabled.store(true, std::memory_order_release); }
   void Scheduler::preempt_enable()  { iss.preempt_disabled.store(false, std::memory_order_release); port_preempt_enable(); }
   // --- End Section: Scheduler public methods ---

   // --- Start Section: Thread public methods ---
   Thread::Thread(Entry&& entry, std::span<std::byte> stack, Priority priority)
   {
      assert(priority < IDLE_PRIORITY);

      auto id = iss.next_thread_id.fetch_add(1, std::memory_order_relaxed);
      LOG_THREAD("Thread::Thread() initialise @ID(%u)", id);

      StackLayout slayout(stack, 0);
      tcb = ::new (slayout.tcb) TaskControlBlock(id, priority, slayout.user_stack, std::move(entry));

      port_context_init(tcb->context(), slayout.user_stack.data(), slayout.user_stack.size(), thread_launcher, tcb);
      set_task_ready(tcb);

      LOG_SCHED_READY_MATRIX();
   }

   Thread::~Thread()
   {
      if (!tcb) return;
      assert(tcb->state == TaskControlBlock::State::Terminated && "Destroying an active thread!");
      LOG_THREAD("Thread::~Thread() destroy @ID(%u)", tcb->id);
      port_context_destroy(tcb->context());
      tcb->~TaskControlBlock();
      tcb = nullptr;
   }

   [[nodiscard]] Thread::Id Thread::get_id() const noexcept
   {
      return tcb->id;
   }

   void Thread::join()
   {
      LOG_THREAD("Thread::join() current thread @ID(%u) requests to join with @ID(%u)", iss.current_task->id, tcb->id);
      if (!tcb) return; // TODO is this state possible? Investigate
      {
         Scheduler::Lock guard;

         assert(iss.current_task && "join() with no current task");
         assert(iss.current_task != tcb && "Thread cannot join itself");
         if (tcb->state == TaskControlBlock::State::Terminated) {
            LOG_THREAD("Thread::join() current thread @ID(%u) has joined with already-terminated @ID(%u)", iss.current_task->id, tcb->id);
            return;
         }

         iss.current_task->state = TaskControlBlock::State::Blocked;
         tcb->join_waiters.push_back(iss.current_task);
         cortos_request_reschedule();
      }
      port_yield();

      assert(tcb->state == TaskControlBlock::State::Terminated);
      LOG_THREAD("Thread::join() current thread @ID(%u) has joined with @ID(%u)", iss.current_task->id, tcb->id);
   }

   std::size_t Thread::reserved_stack_size()
   {
      constexpr auto tcb_size = align_up(sizeof(TaskControlBlock), alignof(TaskControlBlock));
      auto tls_size = 0; // TODO
      return tcb_size + tls_size;
   }
   // --- End Section: Thread public methods ---

   // --- Start Section: Mutex implementation ---
   struct MutexImpl
   {
      // Implicitly initialized to nullptr when opaque is constinit'ed
      TaskControlBlock* owner;
      TaskQueue wait_queue;

      // List of mutexes held by owner
      LinkedListNode<MutexImpl> ml_node;

      constexpr MutexImpl() = default;

      // Assumes preemption is already disabled
      void link_to_owner(TaskControlBlock* new_owner) noexcept
      {
         if (owner == new_owner) return;

         // Unlink from previous owner, if any
         if (owner) {
            if (ml_node.prev) {
               ml_node.prev->ml_node.next = ml_node.next;
            } else {
               owner->held_mutex_head = ml_node.next;
            }
            if (ml_node.next) ml_node.next->ml_node.prev = ml_node.prev;
         }

         owner = new_owner;
         ml_node.next = ml_node.prev = nullptr;

         // Link into new owner's list
         if (owner) {
            ml_node.next = owner->held_mutex_head;
            if (ml_node.next) ml_node.next->ml_node.prev = this;
            owner->held_mutex_head = this;
         }
      }

      // Assumes preemption is already disabled
      bool try_lock_under_guard()
      {
         auto* current = iss.current_task;
         assert(current && "Mutex::try_lock_under_guard() with no current task");
         assert(current != owner && "Recursive mutex lock attempt");

         if (owner != nullptr) {
            LOG_SYNC("Mutex::try_lock_under_guard() @ID(%u) failed to acquire @MUTEX(%p) - owned by @ID(%u)", current->id, ptr_suffix(this), owner->id);
            return false;
         }
         link_to_owner(current);
         LOG_SYNC("Mutex::try_lock_under_guard() @ID(%u) ACQUIRED @MUTEX(%p)", current->id, ptr_suffix(this));
         return true;
      }
   };
   static_assert(std::is_trivially_constructible_v<MutexImpl>, "Reinterpreting opaque block is now UB");
   static_assert(std::is_standard_layout_v<MutexImpl>,         "Reinterpreting opaque block is now UB");
   static_assert(sizeof(MutexImpl) == sizeof(Mutex::ImplStorage),   "Adjust forward size declaration in header to match");
   static_assert(alignof(MutexImpl) == alignof(Mutex::ImplStorage), "Adjust forward align declaration in header to match");

   static void recompute_effective_priority(TaskControlBlock* tcb)
   {
      auto new_priority = tcb->base_priority;
      LOG_SCHED("recompute_effective_priority() @ID(%u) base=%u", tcb->id, new_priority);

      // For each mutex this thread currently owns...
      for (MutexImpl* mutex = tcb->held_mutex_head; mutex; mutex = mutex->ml_node.next) {
         LOG_SCHED("  scanning held @MUTEX(%p) for waiters", ptr_suffix(mutex));
         // ...scan waiters and inherit the highest priority
         for (TaskControlBlock* waiter = mutex->wait_queue.front(); waiter; waiter = waiter->tq_node.next) {
            LOG_SCHED("    waiter @ID(%u) @PRIO(%u)", waiter->id, waiter->priority);
            new_priority = std::min(waiter->priority, new_priority);
         }
      }
      LOG_SCHED("  effective priority candidate for @ID(%u) is %u", tcb->id, new_priority);
      set_effective_priority(tcb, new_priority);
   }

   static void propagate_priority_to_owner(MutexImpl* mutex, TaskControlBlock* blocking_tcb)
   {
      auto* owner = mutex->owner;
      if (!owner) {
         LOG_SYNC("propagate_priority_to_owner() on @MUTEX(%p) but no owner (blocking @ID(%u))", ptr_suffix(mutex), blocking_tcb->id);
         return;
      }
      LOG_SYNC("propagate_priority_to_owner() @MUTEX(%p) owner @ID(%u @PRIO(%u)) <- blocking @ID(%u @PRIO(%u))", ptr_suffix(mutex), owner->id, owner->priority, blocking_tcb->id, blocking_tcb->priority);
      if (blocking_tcb->priority >= owner->priority) {
         LOG_SYNC("  no bump: owner priority %u already <= blocking %u", owner->priority, blocking_tcb->priority);
         return; // Owner is already as high or higher, nothing to do
      }

      set_effective_priority(owner, blocking_tcb->priority);

      // If owner itself is blocked on another mutex, propagate further
      if (auto** mutex_ptr = std::get_if<MutexImpl*>(&owner->wait_target.sync_prim)) {
         if (*mutex_ptr && (*mutex_ptr)->owner && (*mutex_ptr)->owner != owner) {
            LOG_SYNC("  owner @ID(%u) is blocked on @MUTEX(%p): propagating further", owner->id, ptr_suffix(*mutex_ptr));
            propagate_priority_to_owner(*mutex_ptr, blocking_tcb);
         }
      }
   }

   [[nodiscard]] bool Mutex::is_locked() const noexcept { return self->owner != nullptr; }

   void Mutex::lock()
   {
      {
         Scheduler::Lock guard;
         if (self->try_lock_under_guard()) return;
         LOG_SYNC("Mutex::lock() @ID(%u) BLOCKED on @MUTEX(%p)", iss.current_task->id, ptr_suffix(this));

         iss.current_task->wait_target = self.get();
         iss.current_task->state = TaskControlBlock::State::Blocked;
         self->wait_queue.push_back(iss.current_task);

         // Priority inheritance: boost owner and possibly chain
         propagate_priority_to_owner(self.get(), iss.current_task);
         cortos_request_reschedule();
      }
      port_yield(); // Actually block
   }

   bool Mutex::try_lock()
   {
      Scheduler::Lock guard;
      return self->try_lock_under_guard();
   }

   bool Mutex::try_lock_for(Tick::Delta timeout)
   {
      return try_lock_until(Scheduler::tick_now() + timeout);
   }

   bool Mutex::try_lock_until(Tick deadline)
   {
      Tick::Delta remaining;
      {
         Scheduler::Lock guard;

         if (self->try_lock_under_guard()) return true;
         LOG_SYNC("Mutex::lock() @ID(%u) BLOCKED on @MUTEX(%p)", iss.current_task->id, ptr_suffix(this));

         auto const now = Scheduler::tick_now();
         if (now.has_reached(deadline)) return false;

         iss.current_task->wait_target = self.get();
         iss.current_task->state = TaskControlBlock::State::Blocked; // Will soon be overridden as sleeping
         self->wait_queue.push_back(iss.current_task);
         propagate_priority_to_owner(self.get(), iss.current_task);
         remaining = deadline - now;
      }
      Scheduler::sleep_for(remaining);
      {
         Scheduler::Lock guard;
         bool acquired = iss.current_task->wait_target.acquired;
         iss.current_task->wait_target.clear();
         assert(!acquired || self->owner == iss.current_task); // If the wait target has been acquired, then we MUST be the owner!
         return acquired;
      }
   }

   void Mutex::unlock()
   {
      Scheduler::Lock guard;

      assert(iss.current_task && "Mutex::unlock() with no current task");
      assert(self->owner == iss.current_task && "Mutex::unlock() by non-owner");

      LOG_SYNC("Mutex::unlock() by @ID(%u)", iss.current_task->id);

      if (self->wait_queue.empty()) {
         self->link_to_owner(nullptr);
         recompute_effective_priority(iss.current_task);
         return;
      }

      TaskControlBlock* next_owner = self->wait_queue.pop_highest_priority();
      assert(next_owner);
      iss.sleepers.remove(next_owner); // noop if it wasn't in the heap
      next_owner->wait_target.acquired = true;

      LOG_SYNC("Mutex::unlock() waking waiter @ID(%u) on @MUTEX(%p)", next_owner->id, ptr_suffix(this));

      self->link_to_owner(next_owner);

      next_owner->state = TaskControlBlock::State::Ready;
      auto* old_owner = iss.current_task;
      set_task_ready(next_owner);

      // Old owner may have no more (or lower-prio) waiters across its remaining mutexes
      recompute_effective_priority(old_owner);
   }
   // --- End Section: Mutex implementation ---

   // --- Start Section: Semaphore implementation ---
   struct SemaphoreImpl
   {
      TaskQueue wait_queue;
      // Assumes preemption is already disabled
      bool try_acquire_under_guard(unsigned& counter)
      {
         assert(iss.current_task && "Semaphore::try_acquire_under_guard() with no current task");
         if (counter == 0) {
            LOG_SYNC("Semaphore::try_acquire_under_guard() @ID(%u) failed to acquire. @COUNT(%u)", iss.current_task->id, counter);
            return false;
         }
         --counter;
         LOG_SYNC("try_acquire_under_guard() @ID(%u) ACQUIRED. @COUNT(%u)", iss.current_task->id, counter);
         return true;
      }
   };
   static_assert(std::is_trivially_constructible_v<SemaphoreImpl>, "Reinterpreting opaque block is now UB");
   static_assert(std::is_standard_layout_v<SemaphoreImpl>,         "Reinterpreting opaque block is now UB");
   static_assert(sizeof(SemaphoreImpl) == sizeof(Semaphore::ImplStorage),   "Adjust forward size declaration in header to match");
   static_assert(alignof(SemaphoreImpl) == alignof(Semaphore::ImplStorage), "Adjust forward align declaration in header to match");

   void Semaphore::acquire() noexcept
   {
      {
         Scheduler::Lock guard;

         if (self->try_acquire_under_guard(counter)) return;
         LOG_SYNC("Semaphore::acquire() @ID(%u) BLOCKED on @SEM(%p)", iss.current_task->id, ptr_suffix(this));

         iss.current_task->wait_target = self.get();
         iss.current_task->state = TaskControlBlock::State::Blocked;
         self->wait_queue.push_back(iss.current_task);

         cortos_request_reschedule();
      }
      // Actually block
      port_yield();

      LOG_SYNC("Semaphore::acquire() resumed @ID(%u) with token. @COUNTER(%u)", iss.current_task->id, counter);
   }

   [[nodiscard]] bool Semaphore::try_acquire() noexcept
   {
      Scheduler::Lock guard;
      return (self->try_acquire_under_guard(counter));
   }

   [[nodiscard]] bool Semaphore::try_acquire_for(Tick::Delta timeout) noexcept
   {
      return try_acquire_until(Scheduler::tick_now() + timeout);
   }

   [[nodiscard]] bool Semaphore::try_acquire_until(Tick deadline) noexcept
   {
      Tick::Delta remaining;
      {
         Scheduler::Lock guard;

         if (self->try_acquire_under_guard(counter)) return true;

         auto const now = Scheduler::tick_now();
         if (now.has_reached(deadline)) return false;
         LOG_SYNC("Semaphore::try_acquire_until() @ID(%u) BLOCKED on @SEM(%p) until @ALARM(%u)", iss.current_task->id, ptr_suffix(this), deadline.value());

         iss.current_task->wait_target = self.get();
         iss.current_task->state = TaskControlBlock::State::Blocked; // Will soon be overridden as sleeping
         self->wait_queue.push_back(iss.current_task);
         remaining = deadline - now;
      }
      Scheduler::sleep_for(remaining);
      {
         Scheduler::Lock guard;
         bool acquired = iss.current_task->wait_target.acquired;
         iss.current_task->wait_target.clear();
         return acquired;
      }
   }

   void Semaphore::release(unsigned n) noexcept
   {
      if (n == 0U) return;

      Scheduler::Lock guard;

      LOG_SYNC("Semaphore::release(%u) on @SEM(%p)", n, ptr_suffix(this));

      while (n-- > 0U) {
         // If there is a waiter, wake it and give it the token.
         auto* waiter = self->wait_queue.pop_highest_priority();
         if (waiter) {
            iss.sleepers.remove(waiter); // noop if it wasn't in the heap
            waiter->wait_target.acquired = true;

            LOG_SYNC("Semaphore::release() waking waiter @ID(%u) on @SEM(%p)", waiter->id, ptr_suffix(this));

            waiter->state = TaskControlBlock::State::Ready;
            set_task_ready(waiter);
         } else {
            // No waiters, increment the count.
            ++counter;
            LOG_SYNC("Semaphore::release() incremented @COUNTER(%u -> %u)", counter - 1, counter);
         }
      }
      cortos_request_reschedule();
   }
   // --- End Section: Semaphore implementation ---

   // --- Start Section: ConditionVar implementation ---
   struct ConditionVarImpl
   {
      TaskQueue wait_queue;
   };
   static_assert(std::is_trivially_constructible_v<ConditionVarImpl>, "Reinterpreting opaque block is now UB");
   static_assert(std::is_standard_layout_v<ConditionVarImpl>,         "Reinterpreting opaque block is now UB");
   static_assert(sizeof(ConditionVarImpl) == sizeof(ConditionVar::ImplStorage),   "Adjust forward size declaration in header to match");
   static_assert(alignof(ConditionVarImpl) == alignof(ConditionVar::ImplStorage), "Adjust forward align declaration in header to match");

   void ConditionVar::wait(Mutex& lock) noexcept
   {
      {
         Scheduler::Lock guard;

         assert(iss.current_task && "ConditionVar::wait() with no current task");
         LOG_SYNC("ConditionVar::wait() @ID(%u) on @CV(%p) with @MUTEX(%p)", iss.current_task->id, ptr_suffix(this), ptr_suffix(&lock));
         assert(lock.self->owner == iss.current_task && "ConditionVar::wait() requires caller to own the mutex");

         iss.current_task->wait_target = self.get();
         iss.current_task->state       = TaskControlBlock::State::Blocked;
         self->wait_queue.push_back(iss.current_task);

         // Release the mutex, handing it to next waiter (if any)
         if (lock.self->wait_queue.empty()) {
            lock.self->link_to_owner(nullptr);
            recompute_effective_priority(iss.current_task);
         } else {
            auto* next_owner = lock.self->wait_queue.pop_highest_priority();
            assert(next_owner);
            iss.sleepers.remove(next_owner); // noop if not sleeping
            next_owner->wait_target.acquired = true; // For timed mutex waits

            LOG_SYNC("ConditionVar::wait() handing @MUTEX(%p) to waiter @ID(%u)", ptr_suffix(&lock), next_owner->id);

            lock.self->link_to_owner(next_owner);
            auto* old_owner = iss.current_task;
            set_task_ready(next_owner);

            recompute_effective_priority(old_owner);
         }
         cortos_request_reschedule();
      }
      port_yield(); // Actually block

      lock.lock();
   }

   bool ConditionVar::wait_for(Mutex& lock, Tick::Delta timeout) noexcept
   {
      return wait_until(lock, Scheduler::tick_now() + timeout);
   }

   bool ConditionVar::wait_until(Mutex& lock, Tick deadline) noexcept
   {
      Tick::Delta remaining;
      bool signalled;

      {
         Scheduler::Lock guard;

         assert(iss.current_task && "ConditionVar::wait_until() with no current task");

         auto const now = Scheduler::tick_now();
         if (now.has_reached(deadline)) {
            // As per pthread semantics, when wait times out we must still hold the mutex.
            // We haven't released it yet, so just return false.
            return false;
         }

         LOG_SYNC("ConditionVar::wait_until() @ID(%u) on @CV(%p) with @MUTEX(%p) until @ALARM(%u)", iss.current_task->id, ptr_suffix(this), ptr_suffix(&lock), deadline.value());

         assert(lock.self->owner == iss.current_task && "ConditionVar::wait_until() requires caller to own the mutex");

         // 1) Enqueue on condvar
         iss.current_task->wait_target = self.get();
         iss.current_task->state       = TaskControlBlock::State::Blocked;
         self->wait_queue.push_back(iss.current_task);

         // 2) Release the mutex, handing to next owner if any (same as wait())
         if (lock.self->wait_queue.empty()) {
            lock.self->link_to_owner(nullptr);
            recompute_effective_priority(iss.current_task);
         } else {
            TaskControlBlock* next_owner = lock.self->wait_queue.pop_highest_priority();
            assert(next_owner);
            iss.sleepers.remove(next_owner);
            next_owner->wait_target.acquired = true;

            LOG_SYNC("ConditionVar::wait_until() handing @MUTEX(%p) to waiter @ID(%u)", ptr_suffix(&lock), next_owner->id);

            lock.self->link_to_owner(next_owner);
            next_owner->state = TaskControlBlock::State::Ready;
            auto* old_owner = iss.current_task;
            set_task_ready(next_owner);

            recompute_effective_priority(old_owner);
         }

         remaining = deadline - now;
      }
      Scheduler::sleep_for(remaining);
      {
         Scheduler::Lock guard;
         signalled = iss.current_task->wait_target.acquired;
         iss.current_task->wait_target.clear();
      }
      lock.lock();
      return signalled;
   }

   void ConditionVar::notify_one() noexcept
   {
      Scheduler::Lock guard;

      auto* waiter = self->wait_queue.pop_highest_priority();
      if (!waiter) {
         LOG_SYNC("ConditionVar::notify_one() no waiters on @CV(%p)", ptr_suffix(this));
         return;
      }

      iss.sleepers.remove(waiter);          // If it was in a timed wait
      waiter->wait_target.acquired = true;  // Mark "notified" for timed waits
      LOG_SYNC("ConditionVar::notify_one() waking waiter @ID(%u) on @CV(%p)", waiter->id, ptr_suffix(this));
      set_task_ready(waiter);

      cortos_request_reschedule();
   }

   void ConditionVar::notify_all() noexcept
   {
      Scheduler::Lock guard;

      LOG_SYNC("ConditionVar::notify_all() on @CV(%p)", ptr_suffix(this));

      while (auto* waiter = self->wait_queue.pop_highest_priority()) {
         iss.sleepers.remove(waiter);
         waiter->wait_target.acquired = true;
         LOG_SYNC("ConditionVar::notify_all() waking waiter @ID(%u) on @CV(%p)", waiter->id, ptr_suffix(this));
         set_task_ready(waiter);
      }

      cortos_request_reschedule();
   }
   // --- Start Section: ConditionVar implementation ---

   void WaitTarget::remove(TaskControlBlock* tcb) noexcept
   {
      std::visit([tcb](auto& sync_prim) {
         using SyncPrim = std::remove_reference_t<decltype(sync_prim)>;
         if constexpr (std::is_same_v<SyncPrim, MutexImpl*>) {
            sync_prim->wait_queue.remove(tcb);
            if (sync_prim->owner) recompute_effective_priority(sync_prim->owner);
         } else if constexpr (std::is_same_v<SyncPrim, SemaphoreImpl*> ||
                              std::is_same_v<SyncPrim, ConditionVarImpl*>) {
            sync_prim->wait_queue.remove(tcb);
         }
      }, sync_prim);
      clear();
   }

   // -- Start Section: Timer implementation ---
   Timer::Timer()
   {
      ::new (self.get()) TimerImpl(nullptr, Mode::OneShot, this);
   }

   Timer::Timer(Callback&& cb, Mode mode)
   {
      ::new (self.get()) TimerImpl(std::move(cb), mode, this);
   }

   Timer::Timer(Timer&& other) noexcept
   {
      Scheduler::Lock guard;
      assert(!other.self->armed && "Moving a running Timer is not supported");

      ::new (self.get()) TimerImpl(std::move(*other.self.get()));
      self->heap_index = std::numeric_limits<uint16_t>::max();
   }

   Timer& Timer::operator=(Timer&& other) noexcept
   {
      if (this == &other) return *this;

      Scheduler::Lock guard;

      // Cancel this timer if it is currently armed
      if (self->armed) {
         iss.timers.remove(self.get());
         self->armed      = false;
         self->heap_index = std::numeric_limits<uint16_t>::max();

         if (auto* top = iss.timers.top()) {
            iss.next_expiry_tick.store(top->deadline);
         } else {
            iss.next_expiry_tick.disarm();
         }
      }

      assert(!other.self->armed && "Moving a running Timer is not supported");
      self->mode     = other.self->mode;
      self->cb       = std::move(other.self->cb);
      self->deadline = other.self->deadline;
      self->owner    = this;

      return *this;
   }

   bool Timer::is_running()  const noexcept { return self->armed; }
   bool Timer::is_oneshot()  const noexcept { return self->mode == Mode::OneShot;  }
   bool Timer::is_periodic() const noexcept { return self->mode == Mode::Periodic; }

   void Timer::start_after(Tick::Delta delay) { start_at(Scheduler::tick_now() + delay); }

   void Timer::start_at(Tick deadline)
   {
      Scheduler::Lock guard;

      if (!self->cb) {
         LOG_SCHED("Timer::start_at() called on @TIMER(%p) with empty callback", ptr_suffix(this));
         return;
      }

      if (self->armed) iss.timers.remove(self.get()); // Remove and re-add
      self->deadline   = deadline;
      self->armed      = true;
      iss.timers.push(self.get());

      if (auto* top = iss.timers.top()) {
         iss.next_expiry_tick.store(top->deadline);
      } else {
         iss.next_expiry_tick.disarm();
      }

      LOG_SCHED("Timer::start_at() armed @TIMER(%p) for @ALARM(%u)", ptr_suffix(this), deadline.value());
   }

   void Timer::restart() { start_after(0); }

   void Timer::stop()
   {
      Scheduler::Lock guard;

      if (!self->armed) return; // Can we actually get into this state??

      iss.timers.remove(self.get());
      self->armed      = false;
      self->heap_index = std::numeric_limits<uint16_t>::max();

      if (auto* top = iss.timers.top()) {
         iss.next_expiry_tick.store(top->deadline);
      } else {
         iss.next_expiry_tick.disarm();
      }
      LOG_SCHED("Timer::stop() disarmed @TIMER(%p)", ptr_suffix(this));
   }

   void Timer::set_callback(Callback&& cb)
   {
      Scheduler::Lock guard;
      assert(!self->armed && "Timer::set_callback() only valid when timer is not running");
      self->cb = std::move(cb);
   }

   void Timer::set_mode(Mode mode)
   {
      Scheduler::Lock guard;
      assert(!self->armed && "Timer::set_mode() only valid when timer is not running");
      self->mode = mode;
   }

   void Timer::set_period(Tick::Delta /*ticks*/)
   {
      // TODO
   }
   // -- End Section: Timer implementation ---

   // --- Start Section: Port hooks into the kernel ---
   extern "C" void cortos_on_tick(void)
   {
      LOG_PORT("cortos_on_tick()");
      auto const now = Scheduler::tick_now();
      if (iss.next_wake_tick.due(now) || iss.next_slice_tick.due(now) || iss.next_expiry_tick.due(now)) {
         LOG_PORT("cortos_on_tick() -> %s%s%s", iss.next_wake_tick.due(now) ? "wake_tick due "   : "", iss.next_slice_tick.due(now)  ? "slice_tick due "  : "", iss.next_expiry_tick.due(now) ? "expiry_tick due " : "");
         cortos_request_reschedule();
      }
   }

   extern "C" void cortos_request_reschedule(void)
   {
      LOG_PORT("cortos_request_reschedule()");

      iss.need_reschedule.store(true, std::memory_order_relaxed);
      // Under simulation we should return to the scheduler loop in user context
      // On bare metal, we should pend a software interrupt and return from this ISR
   }
   // --- End Section: Port hooks into the kernel ---

#if CORTOS_SIMULATION
   namespace sim
   {
      void schedule_under_sim()  { schedule(); }
      bool system_is_quiescent() { return iss.ready_matrix.empty() && !iss.next_wake_tick.armed() && !iss.next_expiry_tick.armed(); }
   }
#endif
#if DEBUG_PRINT_ENABLE
   namespace debug
   {
      static void print_ready_matrix()
      {
         std::string s("|");
         for (unsigned p = 0; p < Config::MAX_PRIORITIES; ++p) {
            if (iss.ready_matrix.empty_at(p)) s.append(" 0|");
            else s.append(std::format("{: >2}|", iss.ready_matrix.size_at(p)));
         }
                  LOG_SCHED("ready_matrix table:  HIGH                                       MEDIUM                                          LOW  \n"
      "|---------------------     priority_level: | 0| 1| 2| 3| 4| 5| 6| 7| 8| 9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|\n"
      "|---------------------        ready_count: %s", s.c_str());
      }
   }
#endif

} // namespace cortos
