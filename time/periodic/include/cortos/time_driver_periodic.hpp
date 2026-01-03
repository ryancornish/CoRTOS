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

namespace cortos
{

class PeriodicTickDriver : public ITimeDriver
{
public:
   /**
    * @brief Construct a periodic tick driver
    * @param tick_frequency_hz Tick frequency (e.g., 1000 for 1ms ticks)
    * @param on_timer_tick Callback invoked when timer fires
    * @param arg Callback arbitrary argument
    */
   explicit PeriodicTickDriver(uint32_t tick_frequency_hz, Callback on_timer_tick, void* arg = nullptr);

   ~PeriodicTickDriver() override = default;

   PeriodicTickDriver(PeriodicTickDriver const&)            = delete;
   PeriodicTickDriver& operator=(PeriodicTickDriver const&) = delete;
   PeriodicTickDriver(PeriodicTickDriver&&)                 = delete;
   PeriodicTickDriver& operator=(PeriodicTickDriver&&)      = delete;

   // ITimeDriver interface
   [[nodiscard]] TimePoint now() const override;
   void schedule_wakeup(TimePoint wakeup_time) override;
   void cancel_wakeup() override;
   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const override;
   [[nodiscard]] Duration from_microseconds(uint32_t us) const override;
   void start() override;

   // Extended API for port/ISR integration

   /**
    * @brief Called from timer ISR context
    *
    * This is called by the port layer when the hardware timer fires.
    */
   void on_tick_interrupt();

   /**
    * @brief Get the configured tick frequency
    * @return Tick frequency in Hz
    */
   [[nodiscard]] uint32_t get_tick_frequency_hz() const
   {
      return tick_frequency_hz;
   }

private:
   uint32_t tick_frequency_hz;
   std::atomic<uint64_t> tick_count{0};
   bool started{false};
};

} // namespace cortos

#endif // CORTOS_TIME_DRIVER_PERIODIC_HPP
