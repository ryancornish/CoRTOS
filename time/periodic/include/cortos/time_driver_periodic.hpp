/**
 * @file time_driver_periodic.hpp
 * @brief Periodic tick TimeDriver implementation
 *
 * This is the classic RTOS tick model:
 * - Timer interrupt fires at a fixed frequency (e.g., 1kHz)
 * - Time advances in discrete ticks
 * - Simple, predictable, but less power-efficient than tickless
 *
 * EMBEDDED-SAFE:
 * - No heap allocation (fixed-size slot array)
 * - No mutex (uses port interrupt disable/restore)
 * - ISR-safe callback firing
 * - Deterministic worst-case timing
 *
 * Use this for:
 * - Simple applications
 * - Platforms without high-resolution timers
 * - When determinism is more important than power efficiency
 * - Resource-constrained embedded systems
 */

#ifndef CORTOS_TIME_DRIVER_PERIODIC_HPP
#define CORTOS_TIME_DRIVER_PERIODIC_HPP

#include "cortos/time_driver.hpp"

#include <array>
#include <atomic>

namespace cortos
{

/**
 * @brief Periodic tick time driver for embedded systems
 *
 * This driver integrates with the port layer's timer peripheral to provide
 * OS-level time management. It maintains a fixed-size array of scheduled
 * callbacks and fires them when their deadline arrives.
 *
 * Architecture:
 * - Port configures hardware timer via cortos_port_time_setup(tick_hz)
 * - Port timer ISR calls registered handler (isr_trampoline)
 * - Driver fires all callbacks where deadline <= now()
 *
 * Resource usage:
 * - MAX_SCHEDULED_CALLBACKS * sizeof(Slot) ≈ 16 * 24 = 384 bytes
 * - No heap allocation
 * - No dynamic memory
 *
 * Thread safety:
 * - schedule_at/cancel use IrqGuard (port interrupt disable/restore)
 * - on_timer_isr assumes it's called with interrupts disabled
 *
 * SMP considerations:
 * - Currently assumes single "time core" (core 0)
 * - Future: enqueue requests from other cores via IPI
 *
 * Example usage:
 *   PeriodicTickDriver driver(1000); // 1kHz = 1ms tick
 *   ITimeDriver::set_instance(&driver);
 *   driver.start(); // Enables timer interrupt
 *
 *   // Schedule callback 100ms in future
 *   auto h = driver.schedule_at(TimePoint{100}, my_callback, &data);
 *
 *   // In timer ISR (port calls this):
 *   driver.on_timer_isr(); // Advances time, fires due callbacks
 */
class PeriodicTickDriver final : public ITimeDriver
{
public:
   /**
    * @brief Maximum number of simultaneously scheduled callbacks
    *
    * This is a compile-time limit to avoid heap allocation.
    * If schedule_at() is called when all slots are full, it returns {0}.
    *
    * Adjust based on application needs (default: 16 is reasonable for most uses).
    */
   static constexpr uint32_t MAX_SCHEDULED_CALLBACKS = 16;

   /**
    * @brief Construct periodic tick driver
    * @param tick_frequency_hz Tick frequency in Hz (e.g., 1000 for 1ms ticks)
    *
    * This frequency is passed to cortos_port_time_setup() during start().
    * Must be > 0 (0 would indicate tickless mode, which this driver doesn't support).
    */
   explicit PeriodicTickDriver(uint32_t tick_frequency_hz) noexcept
      : tick_frequency_hz(tick_frequency_hz) {}

   /**
    * @brief Get current time from port layer
    * @return Current monotonic time point (from cortos_port_time_now())
    *
    * Delegates to port layer's hardware timer counter.
    */
   [[nodiscard]] TimePoint now() const noexcept override;

   /**
    * @brief Schedule a callback to fire at a specific time
    * @param tp Absolute time when callback should fire
    * @param cb Callback function (must be ISR-safe, will be called from ISR)
    * @param arg User data passed to callback
    * @return Handle for cancellation, or {0} if cb is nullptr or no slots available
    *
    * Thread safety: Uses IrqGuard (disables interrupts during slot allocation)
    *
    * SMP note: Currently assumes caller is on time core (core 0).
    * Future: non-time-core calls should enqueue request and send IPI.
    *
    * If all MAX_SCHEDULED_CALLBACKS slots are full, returns {0}.
    * Caller should check handle.id != 0 to detect failure.
    *
    * In periodic mode, hardware timer is not rearmed (unlike tickless).
    * The periodic ISR will pick up this callback when time advances.
    */
   [[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept override;

   /**
    * @brief Cancel a scheduled callback
    * @param h Handle returned from schedule_at()
    * @return true if cancelled (was pending), false if already fired or invalid
    *
    * Thread safety: Uses IrqGuard (disables interrupts during slot access)
    *
    * SMP note: Currently assumes caller is on time core.
    *
    * If callback already fired or handle is invalid, returns false.
    * Safe to call multiple times with same handle (subsequent calls return false).
    */
   bool cancel(Handle h) noexcept override;

   /**
    * @brief Convert milliseconds to driver ticks
    * @param ms Milliseconds
    * @return Duration in ticks (rounded up to avoid undersleep)
    *
    * Example: at 1kHz, from_milliseconds(10) = 10 ticks
    * Example: at 1kHz, from_milliseconds(1) = 1 tick (not 0!)
    */
   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const noexcept override;

   /**
    * @brief Convert microseconds to driver ticks
    * @param us Microseconds
    * @return Duration in ticks (rounded up)
    *
    * Example: at 1kHz, from_microseconds(5000) = 5 ticks (5ms)
    * Example: at 1kHz, from_microseconds(1001) = 2 ticks (rounds up)
    */
   [[nodiscard]] Duration from_microseconds(uint32_t us) const noexcept override;
   /**
    * @brief Start the periodic time driver
    *
    * Calls port layer to:
    * 1. Register ISR handler (isr_trampoline)
    * 2. Configure timer hardware (cortos_port_time_setup(tick_frequency_hz))
    * 3. Enable timer interrupt (cortos_port_time_irq_enable())
    *
    * After this, timer ISR will fire at tick_frequency_hz and call on_timer_isr().
    *
    * Idempotent: safe to call multiple times (no-op if already started).
    *
    * ASSERTION: tick_frequency_hz > 0 (periodic mode, not tickless)
    */
   void start() noexcept override;

   /**
    * @brief Stop the periodic time driver
    *
    * Disables timer interrupt and disarms timer.
    * Leaves scheduled callbacks in place (they won't fire until restart).
    *
    * Idempotent: safe to call multiple times (no-op if already stopped).
    */
   void stop() noexcept override;

   /**
    * @brief Timer ISR handler - advances time and fires due callbacks
    *
    * This is called from the port's timer ISR wrapper (via isr_trampoline).
    *
    * Contract:
    * - Must be called with interrupts disabled (ISR context)
    * - Must be called exactly once per timer tick
    *
    * Behavior:
    * 2. Gets current time from cortos_port_time_now()
    * 3. Fires all callbacks where deadline <= current_time
    * 4. Frees slot before invoking callback (avoids reentrancy hazards)
    *
    * No heap allocation, no locks (assumes interrupts already disabled).
    */
   void on_timer_isr() noexcept override;

private:
   /**
    * @brief Scheduled callback slot
    */
   struct Slot
   {
      uint32_t id{0};
      uint64_t when{0};
      Callback cb{nullptr};
      void* arg{nullptr};
   };

   /**
    * @brief ISR trampoline for port layer
    * @param arg Pointer to PeriodicTickDriver instance
    *
    * Port registers this function via cortos_port_time_register_isr_handler().
    * When timer ISR fires, port calls this, which delegates to on_timer_isr().
    */
   static void isr_trampoline(void* arg) noexcept
   {
      static_cast<PeriodicTickDriver*>(arg)->on_timer_isr();
   }

   /**
    * @brief RAII interrupt disable/restore guard
    *
    * Uses port layer's cortos_port_irq_save/restore for critical sections.
    * Guards access to slots array during schedule_at/cancel.
    */
   struct IrqGuard
   {
      uint32_t slot;  // Saved interrupt state

      /**
       * @brief Disable interrupts and save state
       */
      IrqGuard();

      /**
       * @brief Restore interrupt state
       */
      ~IrqGuard();
   };

   /**
    * @brief Fire all callbacks that are due (ISR-safe)
    * @param now_ticks Current time from port layer
    *
    * Called from on_timer_isr() with interrupts disabled.
    * Iterates slots and fires/frees those where deadline <= now_ticks.
    *
    * Frees slot before invoking callback to avoid reentrancy issues
    * (callback could call schedule_at() and reuse the slot).
    */
   void fire_due_isr(uint64_t now_ticks) noexcept;

   uint32_t tick_frequency_hz{0};
   uint32_t next_id{1};
   bool started{false};
   std::array<Slot, MAX_SCHEDULED_CALLBACKS> slots{};
};

} // namespace cortos

#endif // CORTOS_TIME_DRIVER_PERIODIC_HPP
