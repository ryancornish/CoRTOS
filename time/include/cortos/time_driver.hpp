/**
* @file time_driver.hpp
* @brief CoRTOS TimeDriver Interface
*
* The TimeDriver sits between the kernel and hardware timers. It provides:
* - Current time queries
* - Scheduling thread wakeups at specific times
* - Tick vs tickless abstraction
*
* The kernel has NO concept of ticks or timers - it just asks the TimeDriver
* to wake threads at specific times, and the TimeDriver figures out how to
* make that happen (periodic tick, one-shot timer, etc.).
*
* Architecture:
*
*   Kernel (Scheduler): Asks for wakeup at time T
*    -> TimeDriver: Configures hardware timer
*       -> Hardware Timer (port-specific): Interrupt fires
*          -> TimeDriver::on_timer_interrupt(): Calls scheduler
*             -> Kernel (wakes threads)
*/

#ifndef CORTOS_TIME_DRIVER_HPP
#define CORTOS_TIME_DRIVER_HPP

#include <cstdint>
#include <functional>
#include <limits>

namespace cortos
{

/* ============================================================================
* Time Types
* ========================================================================= */

/**
* @brief Monotonic time point (in ticks)
*
* This is an opaque, monotonically increasing value. The actual unit
* (microseconds, milliseconds, ticks) depends on the TimeDriver implementation.
*
* Time points can be compared (<, >, ==) but not subtracted directly.
* Use duration_between() to get the difference.
*/
struct TimePoint
{
   uint64_t value{0};

   constexpr TimePoint() = default;
   constexpr explicit TimePoint(uint64_t v) : value(v) {}

   constexpr bool operator==(TimePoint rhs) const { return value == rhs.value; }
   constexpr bool operator!=(TimePoint rhs) const { return value != rhs.value; }
   constexpr bool operator< (TimePoint rhs) const { return value <  rhs.value; }
   constexpr bool operator<=(TimePoint rhs) const { return value <= rhs.value; }
   constexpr bool operator> (TimePoint rhs) const { return value >  rhs.value; }
   constexpr bool operator>=(TimePoint rhs) const { return value >= rhs.value; }

   static constexpr TimePoint max()
   {
      return TimePoint{std::numeric_limits<uint64_t>::max()};
   }
};

/**
* @brief Duration (difference between two TimePoints)
*
* Represents a span of time in the same units as TimePoint.
*/
struct Duration
{
   uint64_t value{0};

   constexpr Duration() = default;
   constexpr explicit Duration(uint64_t v) : value(v) {}

   constexpr TimePoint operator+(TimePoint tp) const
   {
      return TimePoint{tp.value + value};
   }

   constexpr Duration operator+(Duration rhs) const
   {
      return Duration{value + rhs.value};
   }

   constexpr bool operator==(Duration rhs) const { return value == rhs.value; }
   constexpr bool operator< (Duration rhs) const { return value <  rhs.value; }
};

constexpr TimePoint operator+(TimePoint tp, Duration d)
{
   return TimePoint{tp.value + d.value};
}

constexpr Duration operator-(TimePoint a, TimePoint b)
{
   return Duration{a.value - b.value};
}

/* ============================================================================
* TimeDriver Interface
* ========================================================================= */

/**
* @brief Abstract interface for time management
*
* Implementations provide different timing strategies:
* - Periodic tick (classic RTOS tick interrupt)
* - Tickless (one-shot timer, power-efficient)
* - Simulation (virtual time for testing)
*/
class ITimeDriver
{
public:
   explicit ITimeDriver(std::function<void()>&& on_timer_tick) : on_timer_tick(std::move(on_timer_tick)) {}
   virtual ~ITimeDriver() = default;
   ITimeDriver(ITimeDriver const&)            = delete;
   ITimeDriver& operator=(ITimeDriver const&) = delete;
   ITimeDriver(ITimeDriver&&)            = delete;
   ITimeDriver& operator=(ITimeDriver&&) = delete;

   /**
   * @brief Initialize the time driver
   * @param on_timer_tick Callback to invoke when timer fires
   *
   * The TimeDriver will call this callback from interrupt context when
   * a timer interrupt occurs. The callback should wake threads and reschedule.
   */

   /**
   * @brief Get the current time
   * @return Current monotonic time point
   */
   [[nodiscard]] virtual TimePoint now() const = 0;

   /**
   * @brief Schedule the next timer interrupt
   * @param wakeup_time Absolute time when the timer should fire
   *
   * The TimeDriver should configure the hardware timer to fire at (or slightly
   * before) wakeup_time. If wakeup_time is in the past, fire immediately.
   *
   * For periodic tick: This is a no-op (tick fires at fixed intervals).
   * For tickless: Configures the one-shot timer.
   */
   virtual void schedule_wakeup(TimePoint wakeup_time) = 0;

   /**
   * @brief Cancel any pending timer interrupt
   *
   * For periodic tick: This is a no-op.
   * For tickless: Disables the one-shot timer.
   */
   virtual void cancel_wakeup() = 0;

   /**
   * @brief Convert duration to native units (for user convenience)
   *
   * Example: If the TimeDriver runs at 1kHz, from_milliseconds(10) returns
   * Duration{10} (10 ticks).
   */
   [[nodiscard]] virtual Duration from_milliseconds(uint32_t ms) const = 0;
   [[nodiscard]] virtual Duration from_microseconds(uint32_t us) const = 0;

   /**
   * @brief Start the time driver
   *
   * Enables timer interrupts and starts time flowing.
   * Should only be called once, after init().
   */
   virtual void start() = 0;

   // Singleton access
   static ITimeDriver& get_instance()            { return *instance;  }
   static void set_instance(ITimeDriver* driver) { instance = driver; }

protected:
   std::function<void()> on_timer_tick;

private:
   static ITimeDriver* instance;
};

} // namespace cortos

#endif // CORTOS_TIME_DRIVER_HPP
