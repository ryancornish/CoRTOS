#ifndef CORTOS_TIME_DRIVER_TICKLESS_HPP
#define CORTOS_TIME_DRIVER_TICKLESS_HPP

#include "cortos/time_driver.hpp"

#include <array>
#include <cstdint>

namespace cortos
{

class TicklessDriver final : public ITimeDriver
{
public:
   static constexpr std::uint32_t MAX_SCHEDULED_CALLBACKS = 16;

   explicit TicklessDriver(std::uint32_t /*tick_frequency_hz_unused*/) noexcept {}

   [[nodiscard]] TimePoint now() const noexcept override
   {
      return TimePoint{cortos_port_time_now()};
   }

   [[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept override;
   bool cancel(Handle h) noexcept override;

   [[nodiscard]] Duration from_milliseconds(std::uint32_t ms) const noexcept override;
   [[nodiscard]] Duration from_microseconds(std::uint32_t us) const noexcept override;

   void start() noexcept override;
   void stop() noexcept override;

   void on_timer_isr() noexcept override;

private:
   struct Slot
   {
      std::uint32_t id{0};         // 0 = free
      std::uint64_t when{0};       // absolute deadline in port ticks
      Callback cb{nullptr};
      void*    arg{nullptr};
   };

   static void isr_trampoline(void* arg) noexcept
   {
      static_cast<TicklessDriver*>(arg)->on_timer_isr();
   }

   struct IrqGuard
   {
      std::uint32_t state;
      IrqGuard();
      ~IrqGuard();
   };

   void rearm_earliest_locked() noexcept;
   void fire_due_isr(std::uint64_t now_ticks) noexcept;

   std::uint32_t next_id{1};
   bool started{false};
   std::array<Slot, MAX_SCHEDULED_CALLBACKS> slots{};
};

} // namespace cortos
