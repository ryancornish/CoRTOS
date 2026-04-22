/**
 * @file time_driver_tickless.hpp
 * @brief Tickless (one-shot) TimeDriver implementation
 *
 * This is the power-efficient tickless model:
 * - No periodic timer interrupt
 * - Hardware timer armed only when needed (one-shot mode)
 * - Wakes only when a deadline actually arrives
 * - Ideal for battery-powered devices
 *
 * EMBEDDED-SAFE:
 * - No heap allocation (fixed-size slot array)
 * - No mutex (uses port interrupt disable/restore)
 * - ISR-safe callback firing
 * - Dynamic power management friendly
 *
 * Use this for:
 * - Battery-powered devices
 * - Power-sensitive applications
 * - Platforms with high-resolution one-shot timers
 * - When minimizing interrupt overhead is critical
 *
 * Differences from PeriodicTickDriver:
 * - No fixed tick rate (event-driven)
 * - Hardware timer is rearmed for each deadline
 * - Lower power consumption (no wasted interrupts)
 * - Slightly more complex (dynamic timer management)
 */

#ifndef CORTOS_TIME_DRIVER_TICKLESS_HPP
#define CORTOS_TIME_DRIVER_TICKLESS_HPP

#include "cortos/time_driver.hpp"
#include "cortos/port.h"

#include <array>
#include <cstdint>

namespace cortos
{

class TicklessDriver final : public ITimeDriver
{
public:
   static constexpr uint32_t MAX_SCHEDULED_CALLBACKS = 16;

   explicit TicklessDriver(uint32_t /*tick_frequency_hz_unused*/ = 0) noexcept {}

   [[nodiscard]] TimePoint now() const noexcept override
   {
      return TimePoint{cortos_port_time_now()};
   }

   [[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept override;
   bool cancel(Handle h) noexcept override;

   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const noexcept override;

   [[nodiscard]] Duration from_microseconds(uint32_t us) const noexcept override;

   void start() noexcept override;
   void stop() noexcept override;

   void on_timer_isr() noexcept override;

private:
   struct Slot
   {
      uint32_t id{0};        // 0 = free
      uint64_t when{0};      // absolute deadline in port counter ticks
      Callback cb{nullptr};
      void* arg{nullptr};
   };

   static void isr_trampoline(void* arg) noexcept
   {
      static_cast<TicklessDriver*>(arg)->on_timer_isr();
   }

   struct IrqGuard
   {
      uint32_t state;
      IrqGuard() : state(cortos_port_irq_save()) {}
      ~IrqGuard() { cortos_port_irq_restore(state); }
      IrqGuard(IrqGuard const&) = delete;
      IrqGuard& operator=(IrqGuard const&) = delete;
   };

   void rearm_locked() noexcept;          // recompute earliest and arm/disarm
   void fire_due_isr(uint64_t now) noexcept;

   uint32_t next_id{1};
   bool started{false};
   std::array<Slot, MAX_SCHEDULED_CALLBACKS> slots{};
};

} // namespace cortos

#endif // CORTOS_TIME_DRIVER_TICKLESS_HPP
