/**
 * @file time_driver_periodic.cpp
 * @brief Periodic tick TimeDriver implementation
 */

#include "cortos/time_driver_periodic.hpp"
#include <cassert>

namespace cortos
{

PeriodicTickDriver::PeriodicTickDriver(
   std::function<void()>&& on_timer_tick,
   uint32_t tick_frequency_hz
)
   : ITimeDriver(std::move(on_timer_tick))
   , tick_frequency_hz(tick_frequency_hz)
{
   assert(tick_frequency_hz > 0 && "Tick frequency must be positive");
}

TimePoint PeriodicTickDriver::now() const
{
   return TimePoint{tick_count.load(std::memory_order_acquire)};
}

void PeriodicTickDriver::schedule_wakeup(TimePoint /*wakeup_time*/)
{
   // In periodic mode, we don't configure the timer - it fires at fixed intervals
   // The kernel will check on each tick if any threads need waking
}

void PeriodicTickDriver::cancel_wakeup()
{
   // No-op in periodic mode
}

Duration PeriodicTickDriver::from_milliseconds(uint32_t ms) const
{
   uint64_t ticks = (static_cast<uint64_t>(ms) * tick_frequency_hz) / 1000;
   return Duration{ticks};
}

Duration PeriodicTickDriver::from_microseconds(uint32_t us) const
{
   uint64_t ticks = (static_cast<uint64_t>(us) * tick_frequency_hz) / 1'000'000;
   return Duration{ticks};
}

void PeriodicTickDriver::start()
{
   // Platform-specific: enable timer interrupt
   // This would call into port layer to configure hardware timer
   // For now, assume it's done externally
   started = true;
}

void PeriodicTickDriver::on_tick_interrupt()
{
   tick_count.fetch_add(1, std::memory_order_release);

   if (on_timer_tick)
   {
      on_timer_tick();
   }
}

} // namespace cortos
