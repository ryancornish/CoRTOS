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
#include <array>

namespace cortos
{

class PeriodicTickDriver final : public ITimeDriver {
public:
   static constexpr uint32_t MAX_SCHEDULED_CALLBACKS = 16;

   explicit PeriodicTickDriver(uint32_t tick_frequency_hz) noexcept : tick_frequency_hz(tick_frequency_hz) {}

   [[nodiscard]] TimePoint now() const noexcept override;

   [[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept override;
   bool cancel(Handle h) noexcept override;

   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const noexcept override
   {
      // Round up to avoid undersleep for small values
      uint64_t const ticks = (static_cast<uint64_t>(ms) * tick_frequency_hz + 999) / 1000;
      return Duration{ticks};
   }

   [[nodiscard]] Duration from_microseconds(uint32_t us) const noexcept override
   {
      uint64_t const ticks = (static_cast<uint64_t>(us) * tick_frequency_hz + 999'999) / 1'000'000;
      return Duration{ticks};
   }

   void start() noexcept override;
   void stop() noexcept override;

   void on_timer_isr() noexcept override;

private:
   struct Slot
   {
      uint32_t id{0};        // 0 = free
      uint64_t when{0};      // absolute deadline in port ticks
      Callback cb{nullptr};
      void* arg{nullptr};
   };

   static void isr_trampoline(void* arg) noexcept
   {
      static_cast<PeriodicTickDriver*>(arg)->on_timer_isr();
   }

   // Only mutate slots with interrupts masked on the time core.
   struct IrqGuard
   {
      uint32_t slot;
      IrqGuard();
      ~IrqGuard();
   };

   void fire_due_isr(uint64_t now_ticks) noexcept;

   uint32_t tick_frequency_hz{0};
   uint32_t next_id{1};
   bool started{false};
   std::array<Slot, MAX_SCHEDULED_CALLBACKS> slots{};
};

} // namespace cortos

#endif // CORTOS_TIME_DRIVER_PERIODIC_HPP
