/**
 * @file time_driver_periodic.hpp
 * @brief Periodic tick TimeDriver implementation
 *
 * This is the classic RTOS tick model:
 * - Timer interrupt fires at a fixed frequency (e.g., 1kHz)
 * - Time advances in discrete ticks
 * - Simple, predictable, but less power-efficient than tickless
 *
 * Use this for:
 * - Simple applications
 * - Platforms without high-resolution timers
 * - When determinism is more important than power efficiency
 */

#ifndef CORTOS_TIME_DRIVER_PERIODIC_HPP
#define CORTOS_TIME_DRIVER_PERIODIC_HPP

#include "cortos/time_driver.hpp"
#include <atomic>
#include <array>

namespace cortos
{

class PeriodicTickDriver final : public ITimeDriver
{
public:
   static constexpr uint32_t MAX_EVENTS = 16;

   explicit PeriodicTickDriver(uint32_t tick_hz) noexcept : tick_hz(tick_hz) {}

   [[nodiscard]] TimePoint now() const noexcept override;

   [[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept override;
   bool cancel(Handle h) noexcept override;

   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const noexcept override
   {
      // Duration is in port time units. For periodic this is typically ticks at tick_hz_.
      // Round up to be safe for sleeps/timeouts.
      uint64_t ticks = (uint64_t(ms) * tick_hz + 999) / 1000;
      return Duration{ticks};
   }

   [[nodiscard]] Duration from_microseconds(uint32_t us) const noexcept override
   {
      uint64_t ticks = (uint64_t(us) * tick_hz + 999'999) / 1'000'000;
      return Duration{ticks};
   }

   void start() noexcept override;
   void stop() noexcept override;

   void on_timer_isr() noexcept override;

private:
   struct Slot
   {
      uint32_t id{0};          // 0 = free
      uint64_t when{0};        // in port time units
      Callback cb{nullptr};
      void* arg{nullptr};
   };

   static void isr_trampoline(void* arg) noexcept
   {
      static_cast<PeriodicTickDriver*>(arg)->on_timer_isr();
   }

   // Finds and fires due callbacks. Must be ISR-safe.
   void fire_due_isr(uint64_t now) noexcept;

   // Critical section: IRQ save/restore (single-core safety).
   // For SMP safety, the port’s timer IRQ should be routed to a single core
   // OR these operations must be guarded by an SMP-safe lock (see note below).
   struct IrqGuard
   {
      uint32_t s;
      IrqGuard();
      ~IrqGuard();
   };

   uint32_t tick_hz{0};
   uint32_t next_id{1};
   bool started{false};

   std::array<Slot, MAX_EVENTS> slots{};
};

} // namespace cortos

#endif // CORTOS_TIME_DRIVER_PERIODIC_HPP
