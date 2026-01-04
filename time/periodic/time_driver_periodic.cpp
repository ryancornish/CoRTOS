/**
 * @file time_driver_periodic.cpp
 * @brief Periodic tick TimeDriver implementation
 */

#include "cortos/time_driver_periodic.hpp"
#include "cortos/port.h"

#include <cassert>

namespace cortos
{

[[nodiscard]] TimePoint PeriodicTickDriver::now() const noexcept
{
   return TimePoint{cortos_port_time_now()};
}


ITimeDriver::Handle PeriodicTickDriver::schedule_at(TimePoint tp, Callback cb, void* arg) noexcept
{
   if (!cb) return {};

   IrqGuard guard;

   // Find free slot
   for (auto& slot : slots) {
      if (slot.id == 0) {
         uint32_t id = next_id++;
         if (id == 0) id = next_id++; // avoid 0
         slot.id = id;
         slot.when = tp.value;
         slot.cb = cb;
         slot.arg = arg;
         return Handle{id};
      }
   }
   return {}; // out of slots
}

bool PeriodicTickDriver::cancel(Handle h) noexcept
{
   if (h.id == 0) return false;

   IrqGuard g;

   for (auto& slot : slots) {
      if (slot.id == h.id) {
         slot = Slot{};
         return true;
      }
   }
   return false;
}

void PeriodicTickDriver::start() noexcept
{
   // Register handler before enabling IRQ
   cortos_port_time_register_isr_handler(&isr_trampoline, this);
   cortos_port_time_irq_enable();
   started = true;
}

void PeriodicTickDriver::stop() noexcept
{
   cortos_port_time_irq_disable();
   started = false;
}

void PeriodicTickDriver::on_timer_isr() noexcept {
   // In periodic mode, port is expected to deliver this at fixed tick rate.
   // Now comes from port and is already advanced appropriately.
   uint64_t now = cortos_port_time_now();
   fire_due_isr(now);
   // No need to arm one-shot in periodic mode.
}

void PeriodicTickDriver::fire_due_isr(uint64_t now) noexcept {
   // ISR-safe: no heap, no locks (assuming IRQ context is exclusive for this driver).
   // If you allow schedule_at/cancel concurrently from other cores in SMP,
   // you must ensure those operations synchronize (see SMP note).
   for (auto& slot : slots) {
      if (slot.id != 0 && slot.when <= now) {
         auto callback = slot.cb;
         auto arg = slot.arg;
         slot = Slot{}; // free slot before invoking callback (prevents reentrancy issues)
         callback(arg);
      }
   }
}

PeriodicTickDriver::IrqGuard::IrqGuard() : s(cortos_port_irq_save()) {}
PeriodicTickDriver::IrqGuard::~IrqGuard() { cortos_port_irq_restore(s); }

} // namespace cortos
