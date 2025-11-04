/**
 *
 */
#include <cassert>
#include <cornishrtk.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <array>

namespace rtk
{

   struct TaskControlBlock
   {
      enum class State : uint8_t {
         Ready,
         Running,
         Sleeping,
         Blocked,
      } state{State::Ready};
      uint8_t priority{0};
      uint32_t timeslice_left{0};
      uint32_t wake_tick{0};

      port_context_t* context{nullptr};
      void*  stack_base{nullptr};
      size_t stack_size{0};
      Thread::EntryFunction entry_fn;
      void*  arg{nullptr};
   };

   struct InternalSchedulerState
   {
      std::atomic<uint32_t> tick{0};
      std::atomic<bool> preempt_disabled{false};
      std::atomic<bool> need_reschedule{false};
      TaskControlBlock* current_task{nullptr};

      std::array<std::deque<TaskControlBlock*>, MAX_PRIORITIES> ready{}; // TODO: replace deque
      uint32_t ready_bitmap{0};
      std::deque<TaskControlBlock*> sleep_queue{}; // TODO: replace with wheel/heap later

      void reset()
      {
         tick.store(0);
         preempt_disabled.store(false);
         need_reschedule.store(false);
         current_task = nullptr;
      }
   };
   static /*constinit*/ InternalSchedulerState iss;

   static TaskControlBlock* pick_highest_ready()
   {
      if (!iss.ready_bitmap) return nullptr;
      uint8_t priority = std::countr_zero(iss.ready_bitmap);
      return iss.ready[priority].front();
   }

   static void remove_ready_head(uint8_t priority)
   {
      auto& queue = iss.ready[priority];
      queue.pop_front();
      if (queue.empty()) iss.ready_bitmap &= ~(1u << priority);
   }

   static void set_ready(TaskControlBlock* tcb)
   {
      tcb->state = TaskControlBlock::State::Ready;
      iss.ready[tcb->priority].push_back(tcb);
      iss.ready_bitmap |= (1u << tcb->priority);
   }

   static void context_switch_to(TaskControlBlock* next)
   {
      if (next == iss.current_task) return;
      TaskControlBlock* previous_task = iss.current_task;
      iss.current_task = next;
      if (previous_task) previous_task->state = TaskControlBlock::State::Ready;
      iss.current_task->state = TaskControlBlock::State::Running;
      port_switch(previous_task ? &previous_task->context : nullptr, iss.current_task->context);
   }

   static void schedule()
   {
      if (iss.preempt_disabled.load()) return;
      if (!iss.need_reschedule.exchange(false)) return;

      auto next = pick_highest_ready();
      remove_ready_head(next->priority);
      context_switch_to(next);
   }

   void Scheduler::init(uint32_t tick_hz)
   {
      iss.reset();
      port_init(tick_hz);
   }

   void Scheduler::start()
   {
      auto first = pick_highest_ready();
      assert(first != nullptr);
      remove_ready_head(first->priority);
      iss.current_task = first;
      iss.current_task->state = TaskControlBlock::State::Running;
      iss.current_task->timeslice_left = TIME_SLICE;

      port_start_first(iss.current_task->context);

      if constexpr (RTK_SIMULATION) {
         while (true) {
            schedule();
            struct timespec req{.tv_nsec = 1'000'000};
            nanosleep(&req, nullptr);
         }
      }
      __builtin_unreachable();
   }

   void Scheduler::yield()
   {
      rtk_request_reschedule();
      // In sim, returning to the scheduler loop requires the port thread to .resume()
      port_yield();
   }

   uint32_t Scheduler::tick_now()
   {
      return port_tick_now();
   }

   void Scheduler::sleep_for(uint32_t ticks)
   {
      if (ticks == 0) { yield(); return; }
      // Put current to sleep
      assert(iss.current_task && "no current thread");
      iss.current_task->state = TaskControlBlock::State::Sleeping;
      iss.current_task->wake_tick = tick_now() + ticks;
      iss.sleep_queue.push_back(iss.current_task);
      rtk_request_reschedule();
      port_yield();
   }

   void Scheduler::preempt_disable() { port_preempt_disable(); }
   void Scheduler::preempt_enable()  { port_preempt_enable();  }

   static port_context_t* alloc_port_context()
   {
      size_t const size      = port_context_size();
      size_t const alignment = port_stack_align();
      size_t const allocate  = ((size + alignment - 1) / alignment) * alignment;

      return static_cast<port_context_t*>(std::aligned_alloc(alignment, allocate));
   }
   static void free_port_context(port_context_t* port_context_handle)
   {
      free(port_context_handle);
   }

   static void thread_trampoline(void* arg_void)
   {
      auto tcb = static_cast<TaskControlBlock*>(arg_void);
      tcb->entry_fn(tcb->arg);
      // If user function returns, park/surrender forever (could signal joiners later)
      for (;;) { Scheduler::yield(); }
   }

   Thread::Thread(EntryFunction fn, void* arg, void* stack_base, std::size_t stack_size, uint8_t priority)
   {
      assert(priority < MAX_PRIORITIES);
      tcb = new TaskControlBlock{
         .priority = priority,
         .context = alloc_port_context(),
         .stack_base = stack_base,
         .stack_size = stack_size,
         .entry_fn = fn,
         .arg = arg,
      };

      port_context_init(tcb->context, stack_base, stack_size, thread_trampoline, tcb);

      set_ready(tcb);
   }

   Thread::~Thread() {
      // Not handling live-thread destruction yet
      if (!tcb) return;
      free_port_context(tcb->context);
      delete tcb;
   }

   extern "C" void rtk_on_tick(void)
   {
      // Monotonic tick
      iss.tick.fetch_add(1, std::memory_order_relaxed);

      // Wake sleepers whose time expired
      // (linear walk now, replace with timer wheel later)
      uint32_t const now = Scheduler::tick_now();
      for (auto it = iss.sleep_queue.begin(); it != iss.sleep_queue.end();) {
         TaskControlBlock* tcb = *it;
         if (static_cast<int32_t>(now - tcb->wake_tick) >= 0) {
            it = iss.sleep_queue.erase(it);
            set_ready(tcb);
         } else {
            ++it;
         }
      }

      // Round-robin slice accounting for current thread (if any)
      if (iss.current_task && iss.current_task->state == TaskControlBlock::State::Running) {
         if (iss.current_task->timeslice_left > 0) {
               --iss.current_task->timeslice_left;
         }
         if (iss.current_task->timeslice_left == 0) {
            // Move to tail of its ready queue if peers exist
            auto& queue = iss.ready[iss.current_task->priority];
            if (!queue.empty()) {
               queue.push_back(iss.current_task);
               // Remove whichever is at head. we’ll re-pick in reschedule path
               // (Do not remove current from head here. current may not be in q.)
               iss.need_reschedule.store(1, std::memory_order_relaxed);
               iss.current_task->timeslice_left = TIME_SLICE;
            }
         }
      }

      // If any higher-prio thread is ready, request reschedule
      if (iss.ready_bitmap) {
         uint8_t best = std::countr_zero(iss.ready_bitmap);
         if (!iss.current_task || best < iss.current_task->priority) {
               iss.need_reschedule.store(1, std::memory_order_relaxed);
         }
      }
   }

   extern "C" void rtk_request_reschedule(void)
   {
      iss.need_reschedule.store(1, std::memory_order_relaxed);
      // Under simulation we should return to the scheduler loop in user context
      // On bare metal, we should pend a software interrupt and return from this ISR
   }


} // namespace rtk
