#include "scheduler.hpp"

namespace cortos
{

void Scheduler::pin_task(TaskControlBlock& tcb)
{
   tcb.pinned_core = core_id;
   pinned_task_counter.fetch_add(1, std::memory_order_relaxed);
}

void Scheduler::init_idle_task()
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
void Scheduler::start() noexcept
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

void Scheduler::set_task_ready(TaskControlBlock& tcb) noexcept
{
   CORTOS_ASSERT_OP(tcb.pinned_core, ==, core_id);

   tcb.state = TaskControlBlock::State::Ready;

   // Idle task does not belong in the ready_matrix,
   // but DOES follow state transition semantics
   if (&tcb == idle_task) return;

   ready_matrix.enqueue_task(tcb);
}

void Scheduler::drain_inbox() noexcept
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
bool Scheduler::post_to_inbox(CrossCoreRequest request) noexcept
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
void Scheduler::reschedule() noexcept
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

void Scheduler::prepare_block_current_task(std::span<Waitable* const> waitables)
{
   current_task->prepare_block(waitables);
}

Waitable::Result Scheduler::commence_block_current_task()
{
   return current_task->commence_block();
}

void Scheduler::notify_block_current_task(std::span<Waitable* const> waitables) const
{
   current_task->notify_block(waitables);
}

void Scheduler::disable_preemption()
{
   ++preempt_disable_depth;
}

void Scheduler::enable_preemption()
{
   CORTOS_ASSERT(preempt_disable_depth > 0);
   if (--preempt_disable_depth == 0) {
      cortos_port_pend_reschedule(); // TODO: Should this really be here?
   }
}

void Scheduler::reset()
{
   CORTOS_ASSERT_OP(inbox.approx_size(), ==, 0); // Cannot reset whilst inbox is not empty
   CORTOS_ASSERT(ready_matrix.empty()); // Cannot reset whilst tasks still in the queue

   pinned_task_counter.store(0, std::memory_order_relaxed);
   inbox_poke_pending.store(false, std::memory_order_relaxed);
   preempt_disable_depth = 0;
   current_task = nullptr;
}

}  // namespace cortos
