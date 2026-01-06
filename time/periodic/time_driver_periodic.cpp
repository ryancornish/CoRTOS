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

   // SMP Policy A note:
   // For now we assume schedule_at is called on the time core (core 0).
   // Later: non-time-core calls should enqueue a request and poke time core.
   // if (cortos_port_get_core_id() != 0) { enqueue_request(...); cortos_port_send_time_ipi(0); return handle; }

   IrqGuard g;

   for (auto& slot : slots) {
      if (slot.id == 0) {
         uint32_t id = next_id++;
         if (id == 0) id = next_id++; // avoid 0

         slot.id = id;
         slot.when = tp.value;
         slot.cb = cb;
         slot.arg = arg;

         // In periodic mode, we don't arm hardware one-shots.
         // The periodic ISR will pick this up when due.

         return Handle{id};
      }
   }

   return {}; // out of slots
}

bool PeriodicTickDriver::cancel(Handle h) noexcept
{
   if (h.id == 0) return false;

   // Same SMP note as schedule_at regarding time core.
   IrqGuard g;

   for (auto& slot : slots) {
      if (slot.id == h.id) {
         slot = Slot{};
         return true;
      }
   }
   return false;
}

static inline uint64_t ceil_div_u64(uint64_t a, uint64_t b) noexcept { return (a + b - 1) / b; }

[[nodiscard]] Duration PeriodicTickDriver::from_milliseconds(uint32_t ms) const noexcept
{
   const uint64_t f = cortos_port_time_freq_hz();
   const uint64_t ticks = ceil_div_u64(uint64_t(ms) * f, 1000ULL);
   return Duration{ticks};
}

[[nodiscard]] Duration PeriodicTickDriver::from_microseconds(uint32_t us) const noexcept
{
   const uint64_t f = cortos_port_time_freq_hz();
   const uint64_t ticks = ceil_div_u64(uint64_t(us) * f, 1'000'000ULL);
   return Duration{ticks};
}

void PeriodicTickDriver::start() noexcept
{
   if (started) return;

   cortos_port_time_register_isr_handler(&isr_trampoline, this);

   assert(tick_frequency_hz > 0 && "tick_freq of 0 indicates tickless mode, which this driver is NOT!");

   cortos_port_time_setup(tick_frequency_hz);

   cortos_port_time_irq_enable();

   started = true;
}

void PeriodicTickDriver::stop() noexcept {
   if (!started) return;

   cortos_port_time_irq_disable();
   started = false;
}

void PeriodicTickDriver::on_timer_isr() noexcept
{
   const uint64_t now_ticks = cortos_port_time_now();
   fire_due_isr(now_ticks);
}

void PeriodicTickDriver::fire_due_isr(uint64_t now_ticks) noexcept
{
   // No heap, ISR-safe.
   // We free the slot before invoking callback to avoid reentrancy hazards.
   for (auto& slot : slots) {
      if (slot.id != 0 && slot.when <= now_ticks) {
         auto cb = slot.cb;
         auto arg = slot.arg;
         slot = Slot{};
         cb(arg);
      }
   }
}

PeriodicTickDriver::IrqGuard::IrqGuard() : slot(cortos_port_irq_save()) {}
PeriodicTickDriver::IrqGuard::~IrqGuard() { cortos_port_irq_restore(slot); }

} // namespace cortos
