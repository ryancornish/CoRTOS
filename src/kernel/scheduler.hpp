#ifndef CORTOS_SCHEDULER_HPP
#define CORTOS_SCHEDULER_HPP

#include "mpsc_ring_buffer.hpp"
#include "threading_subsystem.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cortos
{

void idle_thread();

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

   [[nodiscard]] constexpr uint32_t current_task_id() const noexcept
   {
      return current_task ? current_task->id : 0;
   }

   [[nodiscard]] constexpr uint8_t current_task_priority() const noexcept
   {
      return current_task ? current_task->effective_priority : 0;
   }

   [[nodiscard]] uint32_t pinned_task_count() const noexcept
   {
      return pinned_task_counter.load(std::memory_order_relaxed);
   }

   void pin_task(TaskControlBlock& tcb);

   void init_idle_task();

   // Core-local operations (only called on owning core)
   void start() noexcept;

   void set_task_ready(TaskControlBlock& tcb) noexcept;

   void drain_inbox() noexcept;

   // Cross-core safe posting API
   bool post_to_inbox(CrossCoreRequest request) noexcept;

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
   void reschedule() noexcept;

   void prepare_block_current_task(std::span<Waitable* const> waitables);

   Waitable::Result commence_block_current_task();

   void notify_block_current_task(std::span<Waitable* const> waitables) const;

   void disable_preemption();

   void enable_preemption();

   void reset();
};

} // namespace cortos

#endif // CORTOS_SCHEDULER_HPP
