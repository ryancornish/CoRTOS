#include "cortos/time_driver_tickless.hpp"
#include "cortos/port.h"

#include <cstdint>
#include <limits>

namespace cortos
{

[[nodiscard]] Duration TicklessDriver::from_milliseconds(uint32_t ms) const noexcept
{
   const uint64_t f = cortos_port_time_freq_hz();
   const uint64_t ticks = (static_cast<uint64_t>(ms) * f + 999) / 1000;
   return Duration{ticks};
}

[[nodiscard]] Duration TicklessDriver::from_microseconds(uint32_t us) const noexcept
{
   const uint64_t f = cortos_port_time_freq_hz();
   const uint64_t ticks = (static_cast<uint64_t>(us) * f + 999'999) / 1'000'000;
   return Duration{ticks};
}

ITimeDriver::Handle TicklessDriver::schedule_at(TimePoint tp, Callback cb, void* arg) noexcept
{
   if (!cb) return {};

   IrqGuard g;

   // Allocate slot
   for (auto& s : slots) {
      if (s.id == 0) {
         uint32_t id = next_id++;
         if (id == 0) id = next_id++;

         s.id = id;
         s.when = tp.value;
         s.cb = cb;
         s.arg = arg;

         // If already due, we don't call cb here (could be with IRQs masked).
         // We'll rely on the next on_timer_isr() call. But we should ensure
         // the timer is armed "immediately" by arming to <= now.
         rearm_locked();

         return Handle{id};
      }
   }

   return {}; // out of slots
}

bool TicklessDriver::cancel(Handle h) noexcept
{
   if (h.id == 0) return false;

   IrqGuard g;

   for (auto& s : slots) {
      if (s.id == h.id) {
         s = Slot{};
         rearm_locked();
         return true;
      }
   }

   return false;
}

void TicklessDriver::start() noexcept
{
   if (started) return;

   cortos_port_time_register_isr_handler(&isr_trampoline, this);

   // tick_hz==0 => tickless setup by your chosen convention
   cortos_port_time_setup(0);

   // Arm based on any pre-existing slots (usually none)
   {
      IrqGuard g;
      rearm_locked();
   }

   cortos_port_time_irq_enable();
   started = true;
}

void TicklessDriver::stop() noexcept
{
   if (!started) return;

   cortos_port_time_irq_disable();
   cortos_port_time_disarm();
   started = false;
}

void TicklessDriver::on_timer_isr() noexcept
{
   const uint64_t now_ticks = cortos_port_time_now();

   // Fire everything due. We free slot before calling cb to avoid reentrancy hazards.
   fire_due_isr(now_ticks);

   // Rearm next deadline after firing
   IrqGuard g;
   rearm_locked();
}

void TicklessDriver::fire_due_isr(uint64_t now) noexcept
{
   // ISR context: do not allocate.
   for (auto& s : slots) {
      if (s.id != 0 && s.when <= now) {
         auto cb = s.cb;
         auto arg = s.arg;
         s = Slot{};
         cb(arg);
      }
   }
}

void TicklessDriver::rearm_locked() noexcept
{
   uint64_t earliest = std::numeric_limits<uint64_t>::max();

   for (auto const& s : slots) {
      if (s.id != 0 && s.when < earliest) {
         earliest = s.when;
      }
   }

   if (earliest == std::numeric_limits<uint64_t>::max()) {
      cortos_port_time_disarm();
   } else {
      // Arm at earliest absolute counter tick. If already in the past, port
      // should arrange an "immediate" interrupt (or next pump on Linux).
      cortos_port_time_arm(earliest);
   }
}

} // namespace cortos
