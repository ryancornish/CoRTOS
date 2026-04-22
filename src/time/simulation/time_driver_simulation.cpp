#include "cortos/time_driver_simulation.hpp"

#include <algorithm>
#include <chrono>

namespace cortos
{

template<TimeMode Mode>
TimePoint SimulationTimeDriver<Mode>::now() const noexcept
{
   if constexpr (Mode == TimeMode::Virtual) {
      return TimePoint{virtual_now.load(std::memory_order_relaxed)};
   } else {
      // Pre-start: treat as 0 to satisfy SingletonAccess test.
      if (!started.load(std::memory_order_acquire)) {
         return TimePoint{0};
      }
      auto elapsed = std::chrono::steady_clock::now() - rt_epoch;
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
      if (ns < 0) ns = 0;

      // Convert real time to "ticks" in driver units
      // ticks = ns * tick_hz / 1e9
      uint64_t ticks = (static_cast<uint64_t>(ns) * tick_frequency_hz) / 1'000'000'000ULL;
      return TimePoint{ticks};
   }
}

template<TimeMode Mode>
ITimeDriver::Handle SimulationTimeDriver<Mode>::schedule_at(TimePoint tp, Callback cb, void* arg) noexcept
{
   if (!cb) return {};

   uint32_t id = next_id.fetch_add(1, std::memory_order_relaxed);
   if (id == 0) id = next_id.fetch_add(1, std::memory_order_relaxed);

   {
      std::lock_guard lk(m);
      events.push_back(Event{.id=id, .when=tp.value, .cb=cb, .arg=arg, .cancelled=false});
   }

   // Don’t run callbacks inline; they run when time advances / pump thread ticks.
   return Handle{id};
}

template<TimeMode Mode>
bool SimulationTimeDriver<Mode>::cancel(Handle h) noexcept
{
   if (h.id == 0) return false;

   std::lock_guard lk(m);
   for (auto& e : events) {
      if (e.id == h.id && !e.cancelled) {
         e.cancelled = true;
         return true;
      }
   }
   return false;
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::start() noexcept
{
   // allow multiple start/stop cycles
   started.store(true, std::memory_order_release);

   if constexpr (Mode == TimeMode::RealTime) {
      bool expected = false;
      if (!running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
         return; // already running
      }

      rt_epoch = std::chrono::steady_clock::now();

      rt_thread = std::thread([this] { realtime_thread_main(); });
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::stop() noexcept
{
   if constexpr (Mode == TimeMode::RealTime) {
      bool was = running.exchange(false, std::memory_order_acq_rel);
      if (!was) return;
      if (rt_thread.joinable()) rt_thread.join();
   }
}

template<TimeMode Mode>
void SimulationTimeDriver<Mode>::on_timer_isr() noexcept
{
   const uint64_t now_ticks = now().value;

   std::vector<Event> due;
   {
      std::lock_guard lk(m);
      for (auto& e : events) {
         if (e.id != 0 && !e.cancelled && e.when <= now_ticks) {
            due.push_back(e);
            e.cancelled = true; // consume
         }
      }
      // Optional: compact cancelled events periodically (not needed for tests)
   }

   for (auto& e : due) {
      e.cb(e.arg);
   }
}

template<>
void SimulationTimeDriver<TimeMode::Virtual>::advance_to(TimePoint tp)
{
   // Clamp monotonic
   uint64_t cur = virtual_now.load(std::memory_order_relaxed);
   uint64_t tgt = tp.value;
   if (tgt < cur) tgt = cur;

   virtual_now.store(tgt, std::memory_order_release);
   on_timer_isr();
}

template<>
void SimulationTimeDriver<TimeMode::Virtual>::advance_by(Duration d)
{
   uint64_t cur = virtual_now.load(std::memory_order_relaxed);
   advance_to(TimePoint{cur + d.value});
}

template<>
void SimulationTimeDriver<TimeMode::RealTime>::realtime_thread_main()
{
   using namespace std::chrono_literals;

   // Simple poll. Good enough for Linux tests; can later sleep-until-next-deadline.
   while (running.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(1ms);
      on_timer_isr();
   }
}

// Explicit instantiations
template class SimulationTimeDriver<TimeMode::RealTime>;
template class SimulationTimeDriver<TimeMode::Virtual>;

} // namespace cortos
