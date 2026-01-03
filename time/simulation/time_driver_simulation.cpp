/**
 * @file time_driver_simulation.cpp
 * @brief Simulation TimeDriver implementation
 */

#include "cortos/time_driver_simulation.hpp"
#include <cassert>

namespace cortos
{

template<TimeMode Mode>
SimulationTimeDriver<Mode>::SimulationTimeDriver(uint32_t tick_frequency_hz, Callback on_timer_tick, void* arg)
   : ITimeDriver(on_timer_tick, arg), tick_frequency_hz(tick_frequency_hz)
{
   if constexpr (Mode == TimeMode::RealTime) {
      start_time = std::chrono::steady_clock::now();
   }
}

template<TimeMode Mode>
SimulationTimeDriver<Mode>::~SimulationTimeDriver()
{
   if (timer_thread.joinable()) {
      timer_thread.join();
   }
}

template<TimeMode Mode>
TimePoint SimulationTimeDriver<Mode>::now() const
{
   if constexpr (Mode == TimeMode::RealTime) {
      // Calculate elapsed time since start
      auto elapsed = std::chrono::steady_clock::now() - start_time;
      auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

      // Convert to ticks
      uint64_t ticks = (static_cast<uint64_t>(us) * tick_frequency_hz) / 1'000'000;
      return TimePoint{ticks};
   } else {
      // Virtual mode: return manually-advanced time
      return TimePoint{virtual_time.load(std::memory_order_acquire)};
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::schedule_wakeup(TimePoint wakeup_time)
{
   next_wakeup.store(wakeup_time.value, std::memory_order_release);

   if constexpr (Mode == TimeMode::RealTime) {
      // In real-time mode, we could spawn a thread to sleep until wakeup_time
      // For simplicity, we rely on the kernel polling now() and checking
      // if wakeup_time has passed
      // A more sophisticated implementation would use condition variables
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::cancel_wakeup()
{
   next_wakeup.store(TimePoint::max().value, std::memory_order_release);
}

template<TimeMode Mode>
Duration SimulationTimeDriver<Mode>::from_milliseconds(uint32_t ms) const
{
   uint64_t ticks = (static_cast<uint64_t>(ms) * tick_frequency_hz) / 1'000;
   return Duration{ticks};
}

template<TimeMode Mode>
Duration SimulationTimeDriver<Mode>::from_microseconds(uint32_t us) const
{
   uint64_t ticks = (static_cast<uint64_t>(us) * tick_frequency_hz) / 1'000'000;
   return Duration{ticks};
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::start()
{
   started = true;

   if constexpr (Mode == TimeMode::RealTime)
   {
      // Start the background timer thread
      timer_thread = std::thread([this]()
      {
         run_timer_loop();
      });
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::advance_time(uint64_t delta_ticks) requires (Mode == TimeMode::Virtual)
{
   uint64_t old_time = virtual_time.fetch_add(delta_ticks, std::memory_order_release);
   uint64_t new_time = old_time + delta_ticks;

   uint64_t wakeup = next_wakeup.load(std::memory_order_acquire);
   if (old_time < wakeup && new_time >= wakeup) {
      if (on_timer_tick) {
         on_timer_tick(arg);
      }
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::advance_to(TimePoint target_time) requires (Mode == TimeMode::Virtual)
{
   uint64_t current = virtual_time.load(std::memory_order_acquire);
   if (target_time.value > current) {
      advance_time(target_time.value - current);
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::run_timer_loop()
{
   // In RealTime mode, periodically check if next_wakeup has passed
   while (started) {
      auto current = now();
      uint64_t wakeup = next_wakeup.load(std::memory_order_acquire);

      if (current.value >= wakeup && wakeup != TimePoint::max().value) {
         // Wakeup time reached
         next_wakeup.store(TimePoint::max().value, std::memory_order_release);

         if (on_timer_tick) {
            on_timer_tick(arg);
         }
      }

      // Sleep for a short interval (simulate tick period)
      std::this_thread::sleep_for(std::chrono::microseconds(1'000'000 / tick_frequency_hz));
   }
}

// Explicit instantiations
template class SimulationTimeDriver<TimeMode::RealTime>;
template class SimulationTimeDriver<TimeMode::Virtual>;


} // namespace cortos
