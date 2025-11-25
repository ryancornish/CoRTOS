/**
 *
 */
#include "cornishrtk.hpp"
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

namespace rtk
{
   static constexpr uint32_t UINT32_BITS = std::numeric_limits<uint32_t>::digits;
   static constexpr uint8_t IDLE_THREAD_ID = 1; // Reserved
   static constexpr Thread::Priority IDLE_PRIORITY(MAX_PRIORITIES - 1);

   static constexpr std::size_t align_down(std::size_t size, std::size_t align) { return size & ~(align - 1); }
   static constexpr std::size_t align_up(std::size_t size, std::size_t align)   { return (size + (align - 1)) & ~(align - 1); }

   struct WaitTarget
   {
      std::variant<std::monostate, MutexImpl*, Semaphore*, ConditionVar*> sync_prim;
      template<typename T> WaitTarget& operator=(T t) noexcept { sync_prim = t; return *this; }
      void clear() noexcept { sync_prim = std::monostate{}; }
      constexpr operator bool() const noexcept { return !std::holds_alternative<std::monostate>(sync_prim); }
      void remove(TaskControlBlock* tcb) noexcept;
   };

   template<typename T> struct LinkedListNode { T* next; T* prev; };

   struct TaskControlBlock
   {
      uint32_t id;
      enum class State : uint8_t { Ready, Running, Sleeping, Blocked } state{State::Ready};
      uint8_t const base_priority;
      uint8_t priority{base_priority};
      Tick    wake_tick{0};
      WaitTarget wait_target;

      std::span<std::byte> stack;
      Thread::Entry entry;

      // Intrusive queue/list links for TaskQueue
      LinkedListNode<TaskControlBlock> tq_node{.next = nullptr, .prev = nullptr};
      uint16_t sleep_index{UINT16_MAX};

      // List of mutexes currently held by me (used for priority inheritance)
      struct MutexImpl* held_mutex_head{nullptr};

      // Opaque, in-place port context storage
      OpaqueImpl<port_context_t, RTK_PORT_CONTEXT_SIZE, RTK_PORT_CONTEXT_ALIGN> context_storage;
      constexpr port_context_t*       context()       noexcept { return context_storage.get(); }
      constexpr port_context_t const* context() const noexcept { return context_storage.get(); }

      TaskControlBlock(uint32_t id, Thread::Priority priority, std::span<std::byte> stack, Thread::Entry entry) :
         id(id), base_priority(priority), stack(stack), entry(entry) {}
   };

   class TaskQueue
   {
      TaskControlBlock* head;
      TaskControlBlock* tail;

   public:
      [[nodiscard]] constexpr bool              empty() const noexcept { return !head; }
      [[nodiscard]] constexpr bool           has_peer() const noexcept { return head && head != tail; }
      [[nodiscard]] constexpr TaskControlBlock* front() const noexcept { return head; }

      // This walks the linked list so isn't 'free'. Will be used for debugging only for now
      [[nodiscard]] size_t size() const noexcept
      {
         std::size_t n = 0;
         for (auto* tcb = head; tcb; tcb = tcb->tq_node.next) ++n;
         return n;
      }

      void push_back(TaskControlBlock* tcb) noexcept
      {
         assert(tcb->tq_node.next == nullptr && tcb->tq_node.prev == nullptr && "TCB already linked");
         tcb->tq_node.next = nullptr;
         tcb->tq_node.prev = tail;
         if (tail) tail->tq_node.next = tcb; else head = tcb;
         tail = tcb;
      }

      TaskControlBlock* pop_front() noexcept
      {
         if (!head) return nullptr;
         auto* tcb = head;
         head = tcb->tq_node.next;
         if (head) head->tq_node.prev = nullptr; else tail = nullptr;
         tcb->tq_node.next = tcb->tq_node.prev = nullptr;
         return tcb;
      }

      void remove(TaskControlBlock* tcb) noexcept
      {
         if (tcb->tq_node.prev) tcb->tq_node.prev->tq_node.next = tcb->tq_node.next; else head = tcb->tq_node.next;
         if (tcb->tq_node.next) tcb->tq_node.next->tq_node.prev = tcb->tq_node.prev; else tail = tcb->tq_node.prev;
         tcb->tq_node.next = tcb->tq_node.prev = nullptr;
      }
   };
   static_assert(std::is_trivially_constructible_v<TaskQueue>, "Must be usable within OpaqueImpl structs");
   static_assert(std::is_standard_layout_v<TaskQueue>,         "Must be usable within OpaqueImpl structs");

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
      std::array<RoundRobinQueue, MAX_PRIORITIES> matrix{};
      uint32_t bitmap{0};
      static_assert(MAX_PRIORITIES <= UINT32_BITS, "bitmap cannot hold that many priorities!");

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

   class SleepMinHeap
   {
      std::array<TaskControlBlock*, MAX_THREADS> heap_buffer{};
      uint16_t size_count{0};

      static uint16_t parent(uint16_t i) noexcept { return (i - 1u) >> 1; }
      static uint16_t left  (uint16_t i) noexcept { return (i << 1) + 1u; }
      static uint16_t right (uint16_t i) noexcept { return (i << 1) + 2u; }

      static bool earlier(const TaskControlBlock* a, const TaskControlBlock* b) noexcept
      {
         return a->wake_tick.is_before(b->wake_tick);
      }
      void swap_nodes(uint16_t a, uint16_t b) noexcept
      {
         std::swap(heap_buffer[a], heap_buffer[b]);
         heap_buffer[a]->sleep_index = a;
         heap_buffer[b]->sleep_index = b;
      }
      void sift_up(uint16_t i) noexcept
      {
         while (i > 0) {
            uint16_t pnt = parent(i);
            if (!earlier(heap_buffer[i], heap_buffer[pnt])) break;
            swap_nodes(i, pnt);
            i = pnt;
         }
      }
      void sift_down(uint16_t i) noexcept
      {
         while (true) {
            uint16_t lft = left(i), rht = right(i), mid = i;
            if (lft < size_count && earlier(heap_buffer[lft], heap_buffer[mid])) mid = lft;
            if (rht < size_count && earlier(heap_buffer[rht], heap_buffer[mid])) mid = rht;
            if (mid == i) break;
            swap_nodes(i, mid);
            i = mid;
         }
      }

   public:
      [[nodiscard]] bool empty() const noexcept { return size_count == 0; }
      [[nodiscard]] uint16_t size() const noexcept { return size_count; }
      [[nodiscard]] TaskControlBlock* top() const noexcept
      {
         return size_count ? heap_buffer[0] : nullptr;
      }

      void push(TaskControlBlock* tcb) noexcept
      {
         uint16_t i = size_count++;
         heap_buffer[i] = tcb;
         tcb->sleep_index = i;
         sift_up(i);
      }
      TaskControlBlock* pop_min() noexcept
      {
         if (!size_count) return nullptr;
         TaskControlBlock* tcb = heap_buffer[0];
         tcb->sleep_index = UINT16_MAX;
         --size_count;
         if (size_count) {
            heap_buffer[0] = heap_buffer[size_count];
            heap_buffer[0]->sleep_index = 0;
            sift_down(0);
         }
         return tcb;
      }
      void remove(TaskControlBlock* tcb) noexcept
      {
         uint16_t i = tcb->sleep_index;
         if (i == UINT16_MAX) return; // Not in heap
         tcb->sleep_index = UINT16_MAX;
         --size_count;
         if (i == size_count) return; // Removed last

         heap_buffer[i] = heap_buffer[size_count];
         heap_buffer[i]->sleep_index = i;

         // Re-heapify from i (either direction)
         if (i > 0 && earlier(heap_buffer[i], heap_buffer[parent(i)])) {
            sift_up(i);
         } else {
            sift_down(i);
         }
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
      TaskControlBlock* idle_tcb{nullptr};

      ReadyMatrix ready_matrix;
      SleepMinHeap sleepers;
      AtomicDeadline next_wake_tick; // When next sleeper must be woken
      AtomicDeadline next_slice_tick; // When the next RR rotation is due
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

   static void context_switch_to(TaskControlBlock* next)
   {
      if (next == iss.current_task) return;
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
         //
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
         iss.next_slice_tick.store(now + TIME_SLICE);
      }

      LOG_SCHED("schedule() pick @ID(%u) @PRIO(%u)", next_task->id, next_task->priority);

      context_switch_to(next_task);
   }

   static void thread_trampoline(void* arg_void)
   {
      auto* tcb = static_cast<TaskControlBlock*>(arg_void);
      tcb->entry();
      // If user function returns, park/surrender forever (could signal joiners later)
      while (true) Scheduler::yield();
   }

   alignas(RTK_STACK_ALIGN) static std::array<std::byte, 2048> idle_stack{}; // TODO: size stack
   static void idle_entry(void*) { while(true) port_idle(); }

   void Scheduler::init(uint32_t tick_hz)
   {
      LOG_SCHED("Scheduler::init(%u hz)", tick_hz);

      port_init(tick_hz);
      // Create the idle thread
      StackLayout slayout(idle_stack, 0);
      iss.idle_tcb = ::new (slayout.tcb) TaskControlBlock(
         IDLE_THREAD_ID,
         IDLE_PRIORITY,
         slayout.user_stack,
         Thread::Entry(idle_entry)
      );

      port_context_init(iss.idle_tcb->context(),
                        slayout.user_stack.data(),
                        slayout.user_stack.size(),
                        thread_trampoline,
                        iss.idle_tcb);
      // Note: we intentionally do NOT call set_task_ready(idle_tcb).
      // Idle is never in ready_matrix.
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
      iss.next_slice_tick.store(Scheduler::tick_now() + TIME_SLICE);
      iss.preempt_disabled.store(false, std::memory_order_release); // Open the scheduler

      port_set_thread_pointer(iss.current_task); // I think this is correct?
      port_start_first(iss.current_task->context());

      if constexpr (RTK_SIMULATION) {
         while (true) {
            schedule();
            struct timespec req{.tv_sec = 0, .tv_nsec = 1'000'000};
            nanosleep(&req, nullptr);
         }
      }
      __builtin_unreachable();
   }

   void Scheduler::yield()
   {
      LOG_SCHED("yield() by @ID(%u)", iss.current_task->id);

      if (iss.current_task && iss.current_task->state == TaskControlBlock::State::Running) {
         set_task_ready(iss.current_task); // put current at tail
      }
      rtk_request_reschedule();
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

      rtk_request_reschedule();
      port_yield();
   }

   void Scheduler::preempt_disable() { port_preempt_disable(); iss.preempt_disabled.store(true, std::memory_order_release); }
   void Scheduler::preempt_enable()  { iss.preempt_disabled.store(false, std::memory_order_release); port_preempt_enable(); }

   Thread::Thread(Entry entry, std::span<std::byte> stack, Priority priority)
   {
      assert(priority < IDLE_PRIORITY);

      auto id = iss.next_thread_id.fetch_add(1, std::memory_order_relaxed);

      StackLayout slayout(stack, 0);
      tcb = ::new (slayout.tcb) TaskControlBlock(id, priority, slayout.user_stack, entry);

      port_context_init(tcb->context(), slayout.user_stack.data(), slayout.user_stack.size(), thread_trampoline, tcb);
      set_task_ready(tcb);

      LOG_SCHED_READY_MATRIX();
   }

   Thread::~Thread()
   {
      if (!tcb) return;
      port_context_destroy(tcb->context());
      tcb->~TaskControlBlock();
      tcb = nullptr;
   }

   [[nodiscard]] Thread::Id Thread::get_id() const noexcept
   {
      return tcb->id;
   }

   std::size_t Thread::reserved_stack_size()
   {
      auto tcb_size = align_up(sizeof(TaskControlBlock), alignof(TaskControlBlock));
      auto tls_size = 0; // TODO
      return tcb_size + tls_size;
   }

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
         assert(current && "Mutex::try_lock() with no current task");

         if (owner != nullptr) return false;
         link_to_owner(current);
         LOG_SYNC("try_lock_under_guard() @ID(%u) ACQUIRED @MUTEX(%p)", current->id, ptr_suffix(this));
         return true;
      }
   };
   static_assert(std::is_trivially_constructible_v<MutexImpl>, "Reinterpreting opaque block is now UB");
   static_assert(std::is_standard_layout_v<MutexImpl>,         "Reinterpreting opaque block is now UB");
   static_assert(sizeof(MutexImpl) == sizeof(Mutex::ImplStorage),   "Adjust forward size declaration in header to match");
   static_assert(alignof(MutexImpl) == alignof(Mutex::ImplStorage), "Adjust forward align declaration in header to match");

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
      if (auto* mutex_ptr = std::get_if<MutexImpl*>(&owner->wait_target.sync_prim)) {
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

         auto* current = iss.current_task;
         assert(self->owner != current && "Mutex::lock() called recursively by owner");

         LOG_SYNC("Mutex::lock() @ID(%u) BLOCKED on @MUTEX(%p)", current->id, ptr_suffix(this));

         current->wait_target = self.get();
         current->state = TaskControlBlock::State::Blocked;
         self->wait_queue.push_back(current);

         // Priority inheritance: boost owner and possibly chain
         propagate_priority_to_owner(self.get(), current);

         rtk_request_reschedule();
      }

      // Actually block
      port_yield();
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
         auto const now = Scheduler::tick_now();
         if (now.has_reached(deadline)) return false;

         auto* current = iss.current_task;
         assert(self->owner != current && "Mutex::lock() called recursively by owner");

         LOG_SYNC("Mutex::try_lock_until() @ID(%u) BLOCKED on @MUTEX(%p)", current->id, ptr_suffix(this));

         current->wait_target = self.get();
         self->wait_queue.push_back(current);
         propagate_priority_to_owner(self.get(), current);
         remaining = deadline - now;
      }
      Scheduler::sleep_for(remaining);
      {
         Scheduler::Lock guard;
         auto* current = iss.current_task;
         return self->owner == current;
      }
   }

   void Mutex::unlock()
   {
      Scheduler::Lock guard;

      auto* current = iss.current_task;
      assert(current && "Mutex::unlock() with no current task");
      assert(self->owner == current && "Mutex::unlock() by non-owner");

      LOG_SYNC("Mutex::unlock() by @ID(%u)", current->id);

      if (self->wait_queue.empty()) {
         self->link_to_owner(nullptr);
         recompute_effective_priority(current);
         return;
      }

      TaskControlBlock* next_owner = self->wait_queue.pop_front();
      assert(next_owner);
      iss.sleepers.remove(next_owner); // noop if it wasn't in the heap
      next_owner->wait_target.clear();

      LOG_SYNC("Mutex::unlock() waking waiter @ID(%u) on @MUTEX(%p)", next_owner->id, ptr_suffix(this));

      self->link_to_owner(next_owner);

      next_owner->state = TaskControlBlock::State::Ready;
      set_task_ready(next_owner);

      // Old owner may have no more (or lower-prio) waiters across its remaining mutexes
      recompute_effective_priority(current);
   }

   void WaitTarget::remove(TaskControlBlock* tcb) noexcept
   {
      std::visit([tcb](auto& sync_prim) {
         using SyncPrim = std::remove_reference_t<decltype(sync_prim)>;
         if constexpr (std::is_same_v<SyncPrim, MutexImpl*>) {
            sync_prim->wait_queue.remove(tcb);
            if (sync_prim->owner) recompute_effective_priority(sync_prim->owner);
         }
      }, sync_prim);
      clear();
   }

   extern "C" void rtk_on_tick(void)
   {
      LOG_PORT("rtk_on_tick()");

      auto const now = Scheduler::tick_now();
      if (iss.next_wake_tick.due(now) || iss.next_slice_tick.due(now)) {

         LOG_PORT("rtk_on_tick() -> %s%s", iss.next_wake_tick.due(now) ? "wake_tick due " : "", iss.next_slice_tick.due(now) ? "slice_tick due" : "");

         rtk_request_reschedule();
      }
   }

   extern "C" void rtk_request_reschedule(void)
   {
      LOG_PORT("rtk_request_reschedule()");

      iss.need_reschedule.store(true, std::memory_order_relaxed);
      // Under simulation we should return to the scheduler loop in user context
      // On bare metal, we should pend a software interrupt and return from this ISR
   }

} // namespace rtk

#if DEBUG_PRINT_ENABLE
static void LOG_SCHED_READY_MATRIX()
{
   std::string s("|");
   for (unsigned p = 0; p < rtk::MAX_PRIORITIES; ++p) {
      if (rtk::iss.ready_matrix.empty_at(p)) s.append(" 0|");
      else s.append(std::format("{: >2}|", rtk::iss.ready_matrix.size_at(p)));
   }
             LOG_SCHED("ready_matrix table:  HIGH                                       MEDIUM                                          LOW  \n"
"|---------------------     priority_level: | 0| 1| 2| 3| 4| 5| 6| 7| 8| 9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|\n"
"|---------------------        ready_count: %s", s.c_str());
}
#endif

