/**
 * @file time_driver_simulation.cpp
 * @brief Simulation TimeDriver implementation
 */

#include "cortos/time_driver_simulation.hpp"
#include <algorithm>
#include <cassert>

namespace cortos
{

template<TimeMode Mode>
SimulationTimeDriver<Mode>::SimulationTimeDriver(uint32_t tick_frequency_hz) : tick_frequency_hz(tick_frequency_hz)
{
   if constexpr (Mode == TimeMode::RealTime) {
      start_time = std::chrono::steady_clock::now();
   }
}

template<TimeMode Mode>
SimulationTimeDriver<Mode>::~SimulationTimeDriver()
{
   if (timer_thread.joinable())
   {
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
ITimeDriver::Handle SimulationTimeDriver<Mode>::schedule_at(TimePoint tp, Callback cb, void* arg) noexcept
{
   std::lock_guard lock(callbacks_mutex);

   uint32_t id = next_handle_id.fetch_add(1, std::memory_order_relaxed);

   scheduled_callbacks.push_back(ScheduledCallback{
      .id = id,
      .when = tp,
      .callback = cb,
      .arg = arg,
      .cancelled = false
   });

   return Handle{id};
}

template<TimeMode Mode>
bool SimulationTimeDriver<Mode>::cancel(Handle h) noexcept
{
   if (h.id == 0) return false;

   std::lock_guard lock(callbacks_mutex);

   for (auto& cb : scheduled_callbacks) {
      if (cb.id == h.id && !cb.cancelled) {
         cb.cancelled = true;
         return true;
      }
   }

   return false;  // Already fired or never existed
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

   if constexpr (Mode == TimeMode::RealTime) {
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

   check_and_fire_callbacks(TimePoint{new_time});
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
   // In RealTime mode, periodically check for scheduled callbacks
   while (started) {
      auto current = now();
      check_and_fire_callbacks(current);

      // Sleep for a short interval (simulate tick period)
      std::this_thread::sleep_for(std::chrono::microseconds(1'000'000 / tick_frequency_hz));
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::check_and_fire_callbacks(TimePoint current_time)
{
   std::lock_guard lock(callbacks_mutex);

   // Find all callbacks that should fire
   for (auto it = scheduled_callbacks.begin(); it != scheduled_callbacks.end(); ) {
      if (!it->cancelled && current_time >= it->when) {
         // Fire callback
         if (it->callback) {
            it->callback(it->arg);
         }

         // Remove from list
         it = scheduled_callbacks.erase(it);
      } else {
         ++it;
      }
   }

   // Clean up cancelled callbacks periodically
   scheduled_callbacks.erase(
      std::remove_if(scheduled_callbacks.begin(), scheduled_callbacks.end(),
         [](const ScheduledCallback& cb) { return cb.cancelled; }),
      scheduled_callbacks.end()
   );
}

// Explicit instantiations
template class SimulationTimeDriver<TimeMode::RealTime>;
template class SimulationTimeDriver<TimeMode::Virtual>;


} // namespace cortos
