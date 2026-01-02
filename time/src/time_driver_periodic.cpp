/**
* @file time_driver_periodic.cpp
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

#include "cortos/time_driver.hpp"

#include <atomic>
#include <cassert>
#include <functional>

namespace cortos
{

class PeriodicTickDriver : public ITimeDriver
{
public:
   /**
   * @brief Construct a periodic tick driver
   * @param tick_frequency_hz Tick frequency (e.g., 1000 for 1ms ticks)
   */
   explicit PeriodicTickDriver(std::function<void()>&& on_timer_tick, uint32_t tick_frequency_hz)
      : ITimeDriver(std::move(on_timer_tick)), tick_frequency_hz(tick_frequency_hz)
   {
      assert(tick_frequency_hz > 0 && "Tick frequency must be positive");
   }

   [[nodiscard]] TimePoint now() const override
   {
      return TimePoint{tick_count.load(std::memory_order_acquire)};
   }

   // In periodic mode, we don't configure the timer - it fires at fixed intervals
   // The kernel will check on each tick if any threads need waking
   void schedule_wakeup(TimePoint /*wakeup_time*/) override {}

   // No-op in periodic mode
   void cancel_wakeup() override {}

   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const override
   {
      uint64_t ticks = (static_cast<uint64_t>(ms) * tick_frequency_hz) / 1000;
      return Duration{ticks};
   }

   [[nodiscard]] Duration from_microseconds(uint32_t us) const override
   {
      uint64_t ticks = (static_cast<uint64_t>(us) * tick_frequency_hz) / 1'000'000;
      return Duration{ticks};
   }

   void start() override
   {
      // Platform-specific: enable timer interrupt
      // This would call into port layer to configure hardware timer
      // For now, assume it's done externally
      started = true;
   }

   /**
   * @brief Called from timer ISR context
   *
   * This is called by the port layer when the hardware timer fires.
   */
   void on_tick_interrupt()
   {
      tick_count.fetch_add(1, std::memory_order_release);
      if (on_timer_tick) on_timer_tick();
   }

   [[nodiscard]] uint32_t get_tick_frequency_hz() const { return tick_frequency_hz; }

private:
   uint32_t tick_frequency_hz;
   std::atomic<uint64_t> tick_count{0};
   bool started{false};
};

ITimeDriver* ITimeDriver::instance = nullptr;

} // namespace cortos
