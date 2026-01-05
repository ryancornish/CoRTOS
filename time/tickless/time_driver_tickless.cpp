#include "cortos/time_driver_tickless.hpp"
#include "cortos/port.h"

#include <cstdint>

namespace cortos
{

TicklessDriver::IrqGuard::IrqGuard() : state(cortos_port_irq_save()) {}
TicklessDriver::IrqGuard::~IrqGuard() { cortos_port_irq_restore(state); }

Duration TicklessDriver::from_milliseconds(uint32_t ms) const noexcept
{
   // Tickless still uses "port ticks" as its unit; conversions are driver-defined.
   // If you want real conversions, you’ll need a "ticks_per_second" concept in the driver.
   // For now, keep the same rounding behaviour you used elsewhere by treating 1 tick = 1ms,
   // OR delete these from tickless until you define the unit. I’ll implement the safe stub:
   return Duration{static_cast<uint64_t>(ms)};
}

Duration TicklessDriver::from_microseconds(uint32_t us) const noexcept
{
   // Same note as above.
   return Duration{(us + 999) / 1000};
}

void TicklessDriver::start() noexcept
{
   if (started) return;

   cortos_port_time_register_isr_handler(&isr_trampoline, this);

   // tick_hz == 0 => tickless
   cortos_port_time_setup(0);

   // Arm earliest if any were scheduled before start()
   {
      IrqGuard g;
      rearm_earliest_locked();
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

ITimeDriver::Handle TicklessDriver::schedule_at(TimePoint tp, Callback cb, void* arg) noexcept
{
   if (!cb) return {};

   IrqGuard g;

   // find free slot
   for (auto& s : slots) {
      if (s.id == 0) {
         uint32_t id = next_id++;
         if (id == 0) id = next_id++; // avoid 0

         s.id   = id;
         s.when = tp.value;
         s.cb   = cb;
         s.arg  = arg;

         // Ensure hardware is armed to earliest deadline
         rearm_earliest_locked();

         // If the earliest deadline is already due, poke the time core to process ASAP.
         // On real embedded, hardware will fire immediately anyway when deadline <= now.
         if (started) {
         uint64_t now = cortos_port_time_now();
         if (tp.value <= now) {
            cortos_port_send_time_ipi(0);
         }
         }

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
         rearm_earliest_locked();
         return true;
      }
   }
   return false;
}

void TicklessDriver::on_timer_isr() noexcept
{
   // Called when armed deadline is reached (or later)
   const uint64_t now_ticks = cortos_port_time_now();

   // Fire everything due
   fire_due_isr(now_ticks);

   // Rearm next deadline (if any)
   {
      IrqGuard g;
      rearm_earliest_locked();
   }
}

void TicklessDriver::fire_due_isr(uint64_t now_ticks) noexcept
{
   // ISR-safe: free slot before invoking callback
   for (auto& s : slots) {
      if (s.id != 0 && s.when <= now_ticks) {
         auto cb  = s.cb;
         auto arg = s.arg;
         s = Slot{};
         cb(arg);
      }
   }
}

void TicklessDriver::rearm_earliest_locked() noexcept
{
   uint64_t earliest = UINT64_MAX;

   for (auto const& s : slots) {
      if (s.id != 0 && s.when < earliest) earliest = s.when;
   }

   if (earliest == UINT64_MAX) {
      cortos_port_time_disarm();
   } else {
      cortos_port_time_arm(earliest);
   }
}

} // namespace cortos
