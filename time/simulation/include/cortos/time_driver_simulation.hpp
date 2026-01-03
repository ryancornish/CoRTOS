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
    * @param on_timer_tick Callback invoked when timer fires
    * @param tick_frequency_hz Simulated tick frequency (e.g., 1000 = 1kHz)
    */
   explicit SimulationTimeDriver(std::function<void()>&& on_timer_tick, uint32_t tick_frequency_hz = 1'000);

   ~SimulationTimeDriver() override;

   SimulationTimeDriver(SimulationTimeDriver const&)            = delete;
   SimulationTimeDriver& operator=(SimulationTimeDriver const&) = delete;
   SimulationTimeDriver(SimulationTimeDriver&&)                 = delete;
   SimulationTimeDriver& operator=(SimulationTimeDriver&&)      = delete;

   // ITimeDriver interface
   [[nodiscard]] TimePoint now() const override;
   void schedule_wakeup(TimePoint wakeup_time) override;
   void cancel_wakeup() override;
   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const override;
   [[nodiscard]] Duration from_microseconds(uint32_t us) const override;
   void start() override;

   // Extended API for tests/simulation

   /**
    * @brief Advance virtual time (Virtual mode only)
    * @param delta_ticks Number of ticks to advance
    */
   void advance_time(uint64_t delta_ticks) requires (Mode == TimeMode::Virtual);

   /**
    * @brief Advance to a specific time (Virtual mode only)
    * @param target_time Target time point
    */
   void advance_to(TimePoint target_time) requires (Mode == TimeMode::Virtual);

private:
   uint32_t tick_frequency_hz;
   std::chrono::steady_clock::time_point start_time;
   std::atomic<uint64_t> virtual_time{0};
   std::atomic<uint64_t> next_wakeup{TimePoint::max().value};
   bool started{false};
   std::thread timer_thread;

   void run_timer_loop();
};

} // namespace cortos

#endif // CORTOS_TIME_DRIVER_SIMULATION_HPP
