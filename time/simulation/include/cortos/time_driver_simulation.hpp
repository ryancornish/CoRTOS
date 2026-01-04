/**
 * @file time_driver_simulation.hpp
 * @brief Simulation TimeDriver for testing
 *
 * This TimeDriver is for use in the Linux simulation port. It provides:
 * - Real-time mode: Uses actual wall-clock time (for realistic simulation)
 * - Virtual time mode: Time only advances when explicitly told (for deterministic tests)
 *
 * Use this for:
 * - Unit testing time-dependent behavior
 * - Debugging race conditions with controlled time
 * - Simulating real-time systems on desktop
 */

#ifndef CORTOS_TIME_DRIVER_SIMULATION_HPP
#define CORTOS_TIME_DRIVER_SIMULATION_HPP

#include "cortos/time_driver.hpp"
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

namespace cortos
{

enum class TimeMode
{
   RealTime,    // Uses actual wall-clock time
   Virtual      // Time only advances when advance_time() is called
};

template<TimeMode Mode>
class SimulationTimeDriver : public ITimeDriver
{
public:
   /**
    * @brief Construct a simulation time driver
    * @param mode RealTime or Virtual
    * @param tick_frequency_hz Simulated tick frequency (e.g., 1000 = 1kHz)
    */
   explicit SimulationTimeDriver(uint32_t tick_frequency_hz);

   ~SimulationTimeDriver() override;

   SimulationTimeDriver(SimulationTimeDriver const&)            = delete;
   SimulationTimeDriver& operator=(SimulationTimeDriver const&) = delete;
   SimulationTimeDriver(SimulationTimeDriver&&)                 = delete;
   SimulationTimeDriver& operator=(SimulationTimeDriver&&)      = delete;

   // ITimeDriver interface
   [[nodiscard]] TimePoint now() const override;
   [[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept override;
   bool cancel(Handle h) noexcept override;
   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const override;
   [[nodiscard]] Duration from_microseconds(uint32_t us) const override;
   void start() override;

   // Extended API for tests/simulation

   /**
    * @brief Advance virtual time (Virtual mode only)
    * @param delta_ticks Number of ticks to advance
    *
    * Fires any callbacks scheduled before new time.
    */
   void advance_time(uint64_t delta_ticks) requires (Mode == TimeMode::Virtual);

   /**
    * @brief Advance to a specific time (Virtual mode only)
    * @param target_time Target time point
    *
    * Fires any callbacks scheduled before target time.
    */
   void advance_to(TimePoint target_time) requires (Mode == TimeMode::Virtual);

private:
   struct ScheduledCallback
   {
      uint32_t  id;
      TimePoint when;
      Callback  callback;
      void*     arg;
      bool      cancelled{false};
   };

   void run_timer_loop();
   void check_and_fire_callbacks(TimePoint current_time);

   uint32_t tick_frequency_hz;
   std::chrono::steady_clock::time_point start_time;
   std::atomic<uint64_t> virtual_time{0};
   bool started{false};
   std::thread timer_thread;

   // Scheduled callbacks
   std::mutex callbacks_mutex;
   std::vector<ScheduledCallback> scheduled_callbacks;
   std::atomic<uint32_t> next_handle_id{1};
};

} // namespace cortos

#endif // CORTOS_TIME_DRIVER_SIMULATION_HPP
