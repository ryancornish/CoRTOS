#ifndef CORTOS_TIME_HPP
#define CORTOS_TIME_HPP

#include <cstdint>
#include <limits>

namespace cortos::time
{

/* ============================================================================
 * Time Types
 * ========================================================================= */

/**
 * @brief Monotonic time point in driver ticks.
 *
 * A TimePoint is an absolute timestamp measured in the logical tick units of
 * the active time driver. The underlying unit is defined by the driver
 * frequency passed to initialise().
 *
 * Time points may be compared directly. Use duration_between() to form a
 * non-negative Duration between two points.
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

   [[nodiscard]] static constexpr TimePoint max()
   {
      return TimePoint{std::numeric_limits<uint64_t>::max()};
   }
};

/**
 * @brief Time duration in driver ticks.
 *
 * A Duration is a relative span of time measured in the same logical units as
 * TimePoint.
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
   constexpr bool operator> (Duration rhs) const { return value >  rhs.value; }
};

/**
 * @brief Add a duration to a time point.
 */
constexpr TimePoint operator+(TimePoint tp, Duration d)
{
   return TimePoint{tp.value + d.value};
}

/**
 * @brief Return the non-negative duration between two time points.
 *
 * If @p a is earlier than @p b, the result is clamped to zero.
 */
constexpr Duration duration_between(TimePoint a, TimePoint b)
{
   return (a.value >= b.value) ? Duration{a.value - b.value} : Duration{0};
}

/**
 * @brief Scheduled callback function type.
 */
using Callback = void(*)(void*);

/**
 * @brief Opaque handle for a scheduled callback.
 *
 * A handle with id == 0 is invalid.
 */
struct Handle
{
   uint32_t id{0};
};


/* ============================================================================
 * Time Driver Interface - Must be implemented by a specific time driver
 * ========================================================================= */

/**
 * @brief Initialise the active time driver.
 *
 * @param frequency_hz Logical driver frequency in Hz.
 *
 * This frequency defines the tick units used by TimePoint, Duration, and the
 * conversion helpers such as from_milliseconds().
 */
void initialise(uint32_t frequency_hz);

/**
 * @brief Finalise the active time driver.
 *
 * Releases any driver-owned state and returns the time subsystem to an
 * uninitialised state.
 */
void finalise();

/**
 * @brief Get the current monotonic time.
 *
 * @return Current time in driver ticks.
 */
[[nodiscard]] TimePoint now() noexcept;

/**
 * @brief Schedule a callback to run at or after a specific time point.
 *
 * @param tp Absolute deadline in driver ticks.
 * @param cb Callback to invoke.
 * @param arg User argument passed to the callback.
 * @return Handle for later cancellation, or an invalid handle on failure.
 *
 * The callback may execute in interrupt context on embedded targets, or in the
 * caller / simulation context depending on the active driver.
 */
[[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept;

/**
 * @brief Cancel a scheduled callback.
 *
 * @param h Handle returned by schedule_at().
 * @return True if the callback was cancelled before firing, false otherwise.
 *
 * It is safe to cancel an invalid, unknown, or already-fired handle.
 */
bool cancel(Handle h) noexcept;

/**
 * @brief Convert milliseconds to a driver Duration.
 *
 * Conversion is rounded up so that non-zero durations do not undersleep.
 */
[[nodiscard]] Duration from_milliseconds(uint32_t ms) noexcept;

/**
 * @brief Convert microseconds to a driver Duration.
 *
 * Conversion is rounded up so that non-zero durations do not undersleep.
 */
[[nodiscard]] Duration from_microseconds(uint32_t us) noexcept;

/**
 * @brief Start the time driver.
 *
 * Enables time progression and any required timer interrupt or background
 * mechanism for the active driver.
 */
void start() noexcept;

/**
 * @brief Stop the time driver.
 *
 * Disables time progression mechanisms used by the active driver.
 */
void stop() noexcept;

/**
 * @brief Timer interrupt handler entry point for the active driver.
 *
 * Called by the port layer when a timer event occurs.
 */
void on_timer_isr() noexcept;

} // namespace cortos::time

#endif // CORTOS_TIME_HPP
