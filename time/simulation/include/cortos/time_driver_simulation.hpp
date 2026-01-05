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
class SimulationTimeDriver final : public ITimeDriver
{
public:
   explicit SimulationTimeDriver(uint32_t tick_frequency_hz) noexcept : tick_frequency_hz(tick_frequency_hz) {}

   ~SimulationTimeDriver() override { stop(); }

   [[nodiscard]] TimePoint now() const noexcept override;

   [[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept override;
   bool cancel(Handle h) noexcept override;

   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const noexcept override
   {
      const uint64_t ticks = (static_cast<uint64_t>(ms) * tick_frequency_hz + 999) / 1000;
      return Duration{ticks};
   }

   [[nodiscard]] Duration from_microseconds(uint32_t us) const noexcept override
   {
      const uint64_t ticks = (static_cast<uint64_t>(us) * tick_frequency_hz + 999'999) / 1'000'000;
      return Duration{ticks};
   }

   void start() noexcept override;
   void stop() noexcept override;

   void on_timer_isr() noexcept override;

   // Virtual-only controls
   void advance_to(TimePoint tp) requires (Mode == TimeMode::Virtual);
   void advance_by(Duration d)  requires (Mode == TimeMode::Virtual);

private:
   struct Event
   {
      uint32_t id{0};
      uint64_t when{0};
      Callback cb{nullptr};
      void* arg{nullptr};
      bool cancelled{false};
   };

   static void isr_trampoline(void* arg) noexcept
   {
      static_cast<SimulationTimeDriver*>(arg)->on_timer_isr();
   }

   void realtime_thread_main() requires (Mode == TimeMode::RealTime);

   uint32_t tick_frequency_hz{0};
   std::atomic<uint32_t> next_id{1};

   std::mutex m;
   std::vector<Event> events;

   std::atomic<bool> running{false};
   std::thread rt_thread;

   std::atomic<uint64_t> virtual_now{0};
   std::chrono::steady_clock::time_point rt_epoch;
   std::atomic<bool> started{false};
};

} // namespace cortos

#endif // CORTOS_TIME_DRIVER_SIMULATION_HPP
