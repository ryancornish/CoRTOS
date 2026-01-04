/**
 * @file time_driver_simulation.cpp
 * @brief Simulation TimeDriver implementation
 */

#include "cortos/time_driver_simulation.hpp"
#include "cortos/port.h"

#include <cassert>

// Provided by the linux backend
extern "C" void cortos_port_time_advance_to(uint64_t t);

namespace cortos
{

template<TimeMode Mode>
TimePoint SimulationTimeDriver<Mode>::now() const noexcept
{
   return TimePoint{cortos_port_time_now()};
}


template<TimeMode Mode>
ITimeDriver::Handle SimulationTimeDriver<Mode>::schedule_at(TimePoint tp, Callback cb, void* arg) noexcept
{
   if (!cb) return {};

   uint32_t id = next_id.fetch_add(1, std::memory_order_relaxed);
   if (id == 0) id = next_id.fetch_add(1, std::memory_order_relaxed);

   std::lock_guard lk(m);
   events.push_back(Event{.id=id, .when=tp.value, .cb=cb, .arg=arg, .cancelled=false});

   // Tickless-readiness: if the port supports arm/disarm, we can arm the earliest deadline.
   // In a pure periodic simulation port, this can be a no-op.
   // We'll keep it simple here: arm the requested deadline (port may take min internally).
   cortos_port_time_arm(tp.value);

   return Handle{id};
}

template<TimeMode Mode>
bool SimulationTimeDriver<Mode>::cancel(Handle h) noexcept
{
   if (h.id == 0) return false;

   std::lock_guard lk(m);
   for (auto& event : events) {
      if (event.id == h.id && !event.cancelled) {
         event.cancelled = true;
         return true;
      }
   }
   return false;
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::start() noexcept
{
   bool expected = false;
   if (!running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

   cortos_port_time_register_isr_handler(&isr_trampoline, this);
   cortos_port_time_irq_enable();

   if constexpr (Mode == TimeMode::RealTime) {
      rt_thread = std::thread([this]{ realtime_thread_main(); });
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::stop() noexcept {
   bool was = running.exchange(false, std::memory_order_acq_rel);
   if (!was) return;

   cortos_port_time_irq_disable();

   if constexpr (Mode == TimeMode::RealTime) {
      if (rt_thread.joinable()) rt_thread.join();
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::on_timer_isr() noexcept {
   const uint64_t now_ticks = cortos_port_time_now();

   std::vector<Event> due;
   {
      std::lock_guard lk(m);
      for (auto& event : events) {
         if (event.id != 0 && !event.cancelled && event.when <= now_ticks) {
            due.push_back(event);
            event.cancelled = true;
         }
      }
      // Optional: compact cancelled events periodically.
   }

   for (auto& event : due) {
      event.cb(event.arg);
   }

   // Tickless-readiness: re-arm next earliest pending event if your port uses it.
   // We can compute min non-cancelled 'when' and call time_arm(min).
   uint64_t next = UINT64_MAX;
   {
      std::lock_guard lk(m);
      for (auto const& event : events) {
         if (event.id != 0 && !event.cancelled && event.when < next) next = event.when;
      }
   }
   if (next == UINT64_MAX) {
      cortos_port_time_disarm();
   } else {
      cortos_port_time_arm(next);
   }
}

template<>
void SimulationTimeDriver<TimeMode::Virtual>::advance_to(TimePoint tp)
{
   // Advance the PORT time (Option 1), then pump ISR once.
   cortos_port_time_advance_to(tp.value);
   on_timer_isr();
}

template<>
void SimulationTimeDriver<TimeMode::Virtual>::advance_by(Duration d)
{
   const uint64_t cur = cortos_port_time_now();
   advance_to(TimePoint{cur + d.value});
}

template<>
void SimulationTimeDriver<TimeMode::RealTime>::realtime_thread_main()
{
   using namespace std::chrono_literals;

   // Simple: poll and pump. You can optimize to sleep until next armed deadline later.
   while (running.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(1ms);
      // In RealTime mode, your PORT time_now() should track steady_clock.
      // The port timer IRQ can be “delivered” by the port itself; but since we have no
      // backend yet, we pump via on_timer_isr() here.
      on_timer_isr();
   }
}

// Explicit instantiations
template class SimulationTimeDriver<TimeMode::RealTime>;
template class SimulationTimeDriver<TimeMode::Virtual>;


} // namespace cortos
