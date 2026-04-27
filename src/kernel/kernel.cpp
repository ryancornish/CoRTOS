// TODO split up the implementation!

#include <cortos/kernel/kernel.hpp>
#include <cortos/port/port.h>
#include <cortos/port/port_traits.h>
#include <cortos/config/config.hpp>

#include "scheduler.hpp"
#include "threading_subsystem.hpp"
#include "wait_subsystem.hpp"
#include "waitable_utilities.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

// Invariants list:
// Only core X mutates Scheduler[X].ready_matrix, current_task, blocked lists, etc.
// Cross-core ops must go through Scheduler::post_to_inbox() + cortos_port_send_reschedule_ipi(core).
// Spinlocks are only used with preemption disabled (and maybe IRQs) inside kernel land.


namespace cortos
{

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

      if (set_thread_ready(tcb) == ReadyAction::Reschedule) {
         cortos_port_pend_reschedule();
      }
   }

   void unregister_thread() noexcept
   {
      CORTOS_ASSERT(active_threads.load(std::memory_order_relaxed) != 0);
      active_threads.fetch_sub(1, std::memory_order_seq_cst);
   }

   /**
   * @brief Make a thread runnable on its pinned core (SMP-safe).
   *
   * Transitions @p tcb to the Ready state and enqueues it on the ready queue
   * of its pinned core. If the thread belongs to another core, a cross-core
   * request is posted so that the owning scheduler performs the enqueue.
   *
   * @param tcb Task control block of the thread to make runnable.
   * @return ReadyAction::Reschedule if the thread was queued on the current
   *         core and has higher priority than the running task, indicating
   *         the caller should request a local reschedule.
   */
   ReadyAction set_thread_ready(TaskControlBlock& tcb) noexcept
   {
      CORTOS_ASSERT_OP(tcb.state, !=, TaskControlBlock::State::Terminated);

      auto& scheduler = scheduler_for_core(tcb.pinned_core);

      // If cores are not running yet, enqueue directly (even for remote cores)
      if (!running.load(std::memory_order_acquire)) {
         scheduler.set_task_ready(tcb);
         return ReadyAction::None;
      }

      auto this_core = cortos_port_get_core_id();
      if (this_core == tcb.pinned_core) {
         scheduler.set_task_ready(tcb);

         if (tcb.is_higher_priority_than(scheduler.current_task_priority())) {
            return ReadyAction::Reschedule;
         }
      } else {
         bool posted = scheduler.post_to_inbox({
            .type = CrossCoreRequest::SetTaskReady,
            .tcb = &tcb
         });
         CORTOS_ASSERT(posted);
      }
      return ReadyAction::None;
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

void thread_launcher(void* tcb_ptr)
{
   auto* tcb = static_cast<TaskControlBlock*>(tcb_ptr);

   cortos_port_set_tls_pointer(tcb); // For now point TLS to the TCB, but in future, TLS sits just after TCB

   tcb->entry();

   tcb->state = TaskControlBlock::State::Terminated;

   // Signal joiners
   tcb->termination.terminate();

   if (tcb->id == Scheduler::IDLE_TASK_ID) return; // Idle tasks are not apart of the same bookkeeping

   KERNEL.unregister_thread();
   cortos_port_thread_exit();
}

void idle_thread()
{
   while (KERNEL.is_running()) {
      cortos_port_idle();
      cortos_port_pend_reschedule();
   }
}

ReadyAction WaitNode::wake_thread(bool acquired) const noexcept
{
   CORTOS_ASSERT(tcb != nullptr);
   CORTOS_ASSERT(group != nullptr);
   CORTOS_ASSERT(waitable != nullptr);
   CORTOS_ASSERT(index != INVALID_INDEX);
   CORTOS_ASSERT_OP(tcb->state, ==, TaskControlBlock::State::Blocked);

   if (!group->try_win(static_cast<int>(index), acquired)) {
      return ReadyAction::None; // lost
   }

   // Winner: remove all nodes in this group (including this one)
   tcb->teardown_wait_group(*group);

   // Thread is ready to be enqueued on its pinned core
   return KERNEL.set_thread_ready(*tcb);
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

void Thread::join() noexcept
{
   CORTOS_ASSERT(tcb != nullptr);

   kernel::wait_until([&]{
      return tcb->termination.has_terminated();
   }, tcb->termination);
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
      {
         WaitableGroupLock lock_group(waitables);

         scheduler.prepare_block_current_task(waitables);
      }
      scheduler.notify_block_current_task(waitables);

      auto result = scheduler.commence_block_current_task();

      return result;
   }

   Waitable::Result wait_until(Waitable::Predicate predicate, std::span<Waitable* const> waitables)
   {
      CORTOS_ASSERT(waitables.size() > 0);
      CORTOS_ASSERT_OP(waitables.size(), <=, config::MAX_WAIT_NODES);
      for (auto* w : waitables) CORTOS_ASSERT(w != nullptr);

      Waitable::Result result{.index = -1, .acquired = false};

      // Fast-path: avoid locks
      if (predicate()) {
         return result;
      }

      auto& scheduler = KERNEL.scheduler_for_this_core();

      while (true) {
         {
            WaitableGroupLock lock_group(waitables);

            if (predicate()) return result;

            scheduler.prepare_block_current_task(waitables);
         }
         scheduler.notify_block_current_task(waitables);

         result = scheduler.commence_block_current_task();
      }
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
