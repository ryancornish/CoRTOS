
#ifndef CORTOS_TIME_HPP
#define CORTOS_TIME_HPP

#include <cstdint>
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
   constexpr bool operator> (Duration rhs) const { return value >  rhs.value; }
};

constexpr TimePoint operator+(TimePoint tp, Duration d)
{
   return TimePoint{tp.value + d.value};
}

constexpr Duration duration_between(TimePoint a, TimePoint b)
{
  return (a.value >= b.value) ? Duration{a.value - b.value} : Duration{0};
}


/* ============================================================================
* TimeDriver Interface
* ========================================================================= */

namespace time
{
   using Callback = void(*)(void*);
   struct Handle { uint32_t id{0}; }; // 0 = invalid

   /**
   * @brief Get the current time
   * @return Current monotonic time point
   */
   [[nodiscard]] TimePoint now() noexcept;

   /**
    * @brief Schedule a callback to run at/after 'tp'.
    * Callback may run in ISR context (real ports) or in the caller context (simulation).
    * @returns a Handle that can be cancelled.
    */
   [[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept;

   /**
    * @brief Cancel a scheduled callback
    * Must be safe if callback already fired or never existed.
    * @returns true if it was cancelled before firing, false otherwise.
    */
   bool cancel(Handle h) noexcept;

   /**
   * @brief Convert duration to native units (for user convenience)
   *
   * Example: If the TimeDriver runs at 1kHz, from_milliseconds(10) returns
   * Duration{10} (10 ticks).
   */
   [[nodiscard]] Duration from_milliseconds(uint32_t ms) noexcept;
   [[nodiscard]] Duration from_microseconds(uint32_t us) noexcept;

   /**
   * @brief Start the time driver
   *
   * Enables timer interrupts and starts time flowing.
   * Should only be called once, after init().
   */
   void start() noexcept;
   void stop() noexcept;

   /**
   * @brief Called in timer interrupt context on the core delivering the timer IRQ.
   */
  void on_timer_isr() noexcept;
}  // namespace time

} // namespace cortos

#endif // CORTOS_TIME_HPP
