/**
* @file time_driver_sim.cpp
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

#include "cortos/time_driver.hpp"

#include <chrono>
#include <atomic>
#include <thread>
#include <cassert>

namespace cortos
{

class SimulationTimeDriver : public ITimeDriver
{
public:
   enum class Mode
   {
      RealTime,    // Uses actual wall-clock time
      Virtual      // Time only advances when advance_time() is called
   };

   /**
   * @brief Construct a simulation time driver
   * @param mode RealTime or Virtual
   * @param tick_frequency_hz Simulated tick frequency (e.g., 1000 = 1kHz)
   */
   explicit SimulationTimeDriver(std::function<void()>&& on_timer_tick, Mode mode = Mode::RealTime, uint32_t tick_frequency_hz = 1'000)
      : ITimeDriver(std::move(on_timer_tick)), mode(mode), tick_frequency_hz(tick_frequency_hz)
   {
      assert(tick_frequency_hz > 0 && "Tick frequency must be positive");

      if (mode == Mode::RealTime) {
         start_time = std::chrono::steady_clock::now();
      }
   }

   SimulationTimeDriver(SimulationTimeDriver const&)            = delete;
   SimulationTimeDriver& operator=(SimulationTimeDriver const&) = delete;
   SimulationTimeDriver(SimulationTimeDriver&&)            = delete;
   SimulationTimeDriver& operator=(SimulationTimeDriver&&) = delete;

   [[nodiscard]] TimePoint now() const override
   {
      if (mode == Mode::RealTime) {
         // Calculate elapsed time since start
         auto elapsed = std::chrono::steady_clock::now() - start_time;
         auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

         // Convert to ticks
         uint64_t ticks = (static_cast<uint64_t>(us) * tick_frequency_hz) / 1'000'000;
         return TimePoint{ticks};
      } else { // Virtual mode: return manually-advanced time
         return TimePoint{virtual_time.load(std::memory_order_acquire)};
      }
   }

   void schedule_wakeup(TimePoint wakeup_time) override
   {
      next_wakeup.store(wakeup_time.value, std::memory_order_release);

      if (mode == Mode::RealTime) {
         // In real-time mode, we could spawn a thread to sleep until wakeup_time
         // For simplicity, we rely on the kernel polling now() and checking
         // if wakeup_time has passed
         // A more sophisticated implementation would use condition variables
      }
   }

   void cancel_wakeup() override
   {
      next_wakeup.store(TimePoint::max().value, std::memory_order_release);
   }

   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const override
   {
      uint64_t ticks = (static_cast<uint64_t>(ms) * tick_frequency_hz) / 1'000;
      return Duration{ticks};
   }

   [[nodiscard]] Duration from_microseconds(uint32_t us) const override
   {
      uint64_t ticks = (static_cast<uint64_t>(us) * tick_frequency_hz) / 1'000'000;
      return Duration{ticks};
   }

   void start() override
   {
      started = true;

      if (mode == Mode::RealTime) {
         // Start the background timer thread
         timer_thread = std::thread([this]() {
            run_timer_loop();
         });
      }
   }

   /**
   * @brief Advance virtual time (Virtual mode only)
   * @param delta_ticks Number of ticks to advance
   *
   * In Virtual mode, time only advances when this is called.
   * This allows deterministic testing of time-dependent behavior.
   */
   void advance_time(uint64_t delta_ticks)
   {
      assert(mode == Mode::Virtual && "advance_time() only valid in Virtual mode");

      uint64_t old_time = virtual_time.fetch_add(delta_ticks, std::memory_order_release);
      uint64_t new_time = old_time + delta_ticks;

      uint64_t wakeup = next_wakeup.load(std::memory_order_acquire);
      if (old_time < wakeup && new_time >= wakeup) {
         if (on_timer_tick) on_timer_tick();
      }
   }

   /**
   * @brief Advance to a specific time (Virtual mode only)
   * @param target_time Target time point
   */
   void advance_to(TimePoint target_time)
   {
      assert(mode == Mode::Virtual && "advance_to() only valid in Virtual mode");

      uint64_t current = virtual_time.load(std::memory_order_acquire);
      if (target_time.value > current) {
         advance_time(target_time.value - current);
      }
   }

   ~SimulationTimeDriver() override
   {
      if (timer_thread.joinable()) {
         timer_thread.join();
      }
   }

private:
   void run_timer_loop()
   {
      // In RealTime mode, periodically check if next_wakeup has passed
      while (started) {
         auto current = now();
         uint64_t wakeup = next_wakeup.load(std::memory_order_acquire);

         if (current.value >= wakeup && wakeup != TimePoint::max().value) {
            // Wakeup time reached
            next_wakeup.store(TimePoint::max().value, std::memory_order_release);

            if (on_timer_tick) on_timer_tick();
         }

         // Sleep for a short interval (simulate tick period)
         std::this_thread::sleep_for(std::chrono::microseconds(1'000'000 / tick_frequency_hz));
      }
   }

   Mode mode;
   uint32_t tick_frequency_hz;
   std::chrono::steady_clock::time_point start_time;
   std::atomic<uint64_t> virtual_time{0};
   std::atomic<uint64_t> next_wakeup{TimePoint::max().value};
   bool started{false};
   std::thread timer_thread;
};

ITimeDriver* ITimeDriver::instance = nullptr;

} // namespace cortos
