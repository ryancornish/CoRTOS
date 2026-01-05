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

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace cortos
{

/**
 * @brief Time progression mode for simulation driver
 */
enum class TimeMode
{
   RealTime,    // Uses actual wall-clock time (for realistic simulation)
   Virtual      // Time only advances when advance_to/advance_by called (deterministic)
};

/**
 * @brief Template-based simulation time driver
 *
 * @tparam Mode TimeMode::RealTime or TimeMode::Virtual
 *
 * RealTime mode:
 *   - time progresses with wall clock
 *   - background thread polls and fires callbacks
 *   - useful for integration testing with realistic timing
 *
 * Virtual mode:
 *   - time only advances when explicitly told
 *   - deterministic, reproducible test behavior
 *   - advance_to() and advance_by() control time progression
 *
 * Thread safety:
 *   - All methods are thread-safe
 *   - Callbacks may fire from ISR thread (RealTime) or caller thread (Virtual)
 *
 * Example (Virtual):
 *   SimulationTimeDriver<TimeMode::Virtual> driver(1000); // 1kHz
 *   driver.start();
 *
 *   auto h = driver.schedule_at(TimePoint{100}, callback, &data);
 *   driver.advance_to(TimePoint{100}); // Fires callback
 *
 * Example (RealTime):
 *   SimulationTimeDriver<TimeMode::RealTime> driver(1000);
 *   driver.start(); // Background thread starts
 *
 *   auto h = driver.schedule_at(TimePoint{100}, callback, &data);
 *   // Callback fires ~100ms later automatically
 */
template<TimeMode Mode>
class SimulationTimeDriver final : public ITimeDriver
{
public:
   /**
    * @brief Construct simulation time driver
    * @param tick_frequency_hz Simulated tick frequency (e.g., 1000 = 1kHz = 1ms ticks)
    *
    * The tick frequency determines the relationship between TimePoint values
    * and real time. For example, at 1kHz, TimePoint{100} = 100ms.
    */
   explicit SimulationTimeDriver(uint32_t tick_frequency_hz) noexcept
      : tick_frequency_hz(tick_frequency_hz) {}

   /**
    * @brief Destructor - stops driver and joins background thread
    */
   ~SimulationTimeDriver() override { stop(); }

   /**
    * @brief Get current time
    * @return Current monotonic time point
    *
    * RealTime: Returns elapsed time since start() in ticks
    * Virtual: Returns manually-advanced virtual time
    */
   [[nodiscard]] TimePoint now() const noexcept override;

   /**
    * @brief Schedule a callback to fire at a specific time
    * @param tp Absolute time when callback should fire
    * @param cb Callback function (must be ISR-safe)
    * @param arg User data passed to callback
    * @return Handle for cancellation, or {0} if cb is nullptr
    *
    * The callback will fire when now() >= tp.
    * In RealTime mode, this happens automatically via background thread.
    * In Virtual mode, this happens when advance_to/advance_by crosses tp.
    */
   [[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept override;

   /**
    * @brief Cancel a scheduled callback
    * @param h Handle returned from schedule_at()
    * @return true if cancelled, false if already fired or invalid handle
    */
   bool cancel(Handle h) noexcept override;

   /**
    * @brief Convert milliseconds to driver ticks
    * @param ms Milliseconds
    * @return Duration in ticks (rounded up)
    */
   [[nodiscard]] Duration from_milliseconds(uint32_t ms) const noexcept override
   {
      const uint64_t ticks = (static_cast<uint64_t>(ms) * tick_frequency_hz + 999) / 1000;
      return Duration{ticks};
   }

   /**
    * @brief Convert microseconds to driver ticks
    * @param us Microseconds
    * @return Duration in ticks (rounded up)
    */
   [[nodiscard]] Duration from_microseconds(uint32_t us) const noexcept override
   {
      const uint64_t ticks = (static_cast<uint64_t>(us) * tick_frequency_hz + 999'999) / 1'000'000;
      return Duration{ticks};
   }

   /**
    * @brief Start the time driver
    *
    * RealTime: Captures epoch and spawns background polling thread
    * Virtual: Sets started flag (time remains at 0 until advance_*)
    *
    * Safe to call multiple times (idempotent in RealTime)
    */
   void start() noexcept override;

   /**
    * @brief Stop the time driver
    *
    * RealTime: Stops and joins background thread
    * Virtual: No-op (but sets flag for consistency)
    */
   void stop() noexcept override;

   /**
    * @brief ISR handler - fires all due callbacks
    *
    * Called automatically in RealTime mode by background thread.
    * Called automatically in Virtual mode by advance_to/advance_by.
    *
    * Fires all callbacks where scheduled_time <= now().
    */
   void on_timer_isr() noexcept override;

   /**
    * @brief Advance virtual time to specific point (Virtual mode only)
    * @param tp Target time point
    *
    * If tp < current_time, time is not moved backward (clamped).
    * Fires all callbacks with scheduled_time <= tp.
    *
    * Only available when Mode == TimeMode::Virtual (compile error otherwise).
    */
   void advance_to(TimePoint tp) requires (Mode == TimeMode::Virtual);

   /**
    * @brief Advance virtual time by duration (Virtual mode only)
    * @param d Duration to advance
    *
    * Equivalent to advance_to(now() + d).
    * Fires all callbacks in the range [old_now, new_now].
    *
    * Only available when Mode == TimeMode::Virtual (compile error otherwise).
    */
   void advance_by(Duration d) requires (Mode == TimeMode::Virtual);

private:
   /**
    * @brief Scheduled callback event
    */
   struct Event
   {
      uint32_t id{0};
      uint64_t when{0};
      Callback cb{nullptr};
      void* arg{nullptr};
      bool cancelled{false};
   };

   /**
    * @brief Trampoline for port-style ISR callback (unused in simulation)
    * @param arg Pointer to SimulationTimeDriver instance
    */
   static void isr_trampoline(void* arg) noexcept
   {
      static_cast<SimulationTimeDriver*>(arg)->on_timer_isr();
   }

   /**
    * @brief Background polling thread main (RealTime mode only)
    *
    * Polls at 1ms intervals and fires due callbacks.
    */
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
