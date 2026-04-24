#include <cortos/time/time.hpp>
#include <cortos/config/config.hpp>
#include <cortos/port/port.h>

#include <array>
#include <cstdint>

namespace cortos::time::periodic
{
   /**
    * @brief Maximum number of simultaneously scheduled callbacks
    *
    * This is a fixed-size embedded-safe limit to avoid heap allocation.
    */
   static constexpr uint32_t MAX_SCHEDULED_CALLBACKS = 16;

   /**
    * @brief Scheduled callback slot
    */
   struct Slot
   {
      uint32_t id{0};
      uint64_t when{0};
      Callback cb{nullptr};
      void* arg{nullptr};
   };

   /**
    * @brief RAII interrupt disable/restore guard
    */
   struct IrqGuard
   {
      uint32_t state;

      IrqGuard() noexcept : state(cortos_port_irq_save()) {}
      ~IrqGuard() { cortos_port_irq_restore(state); }

      IrqGuard(IrqGuard const&) = delete;
      IrqGuard& operator=(IrqGuard const&) = delete;
   };

   /**
    * @brief Internal periodic driver state
    */
   struct DriverState
   {
      bool initialised{false};
      uint32_t tick_frequency_hz{0};
      uint32_t next_id{1};
      bool started{false};
      std::array<Slot, MAX_SCHEDULED_CALLBACKS> slots{};
   };

   static constinit DriverState ds{};

   static void fire_due_isr(uint64_t now_ticks) noexcept
   {
      // No heap, ISR-safe.
      // Free slot before invoking callback to avoid reentrancy hazards.
      for (auto& slot : ds.slots) {
         if (slot.id != 0 && slot.when <= now_ticks) {
            auto cb = slot.cb;
            auto arg = slot.arg;
            slot = Slot{};
            cb(arg);
         }
      }
   }

   static void isr_trampoline(void*) noexcept
   {
      cortos::time::on_timer_isr();
   }

   static inline uint64_t ceil_div_u64(uint64_t a, uint64_t b) noexcept
   {
      return (a + b - 1) / b;
   }
} // namespace cortos::time::periodic


namespace cortos::time
{

void initialise(uint32_t frequency_hz)
{
   CORTOS_ASSERT(!periodic::ds.initialised);
   periodic::ds.initialised = true;
   periodic::ds.tick_frequency_hz = frequency_hz;
}

void finalise()
{
   CORTOS_ASSERT(periodic::ds.initialised);
   periodic::ds = periodic::DriverState{};
}

[[nodiscard]] TimePoint now() noexcept
{
   return TimePoint{cortos_port_time_now()};
}

[[nodiscard]] Handle schedule_at(TimePoint tp, Callback cb, void* arg) noexcept
{
   if (!cb) {
      return {};
   }

   // SMP note:
   // For now this assumes schedule_at() is called on the time core.
   // Future work can enqueue requests to the time core and poke it via IPI.

   periodic::IrqGuard guard;

   for (auto& slot : periodic::ds.slots) {
      if (slot.id == 0) {
         uint32_t id = periodic::ds.next_id++;
         if (id == 0) {
            id = periodic::ds.next_id++; // avoid invalid handle id 0
         }

         slot.id = id;
         slot.when = tp.value;
         slot.cb = cb;
         slot.arg = arg;

         // In periodic mode, no one-shot rearm is required.
         // The periodic ISR will pick this callback up when due.
         return Handle{id};
      }
   }

   return {}; // out of slots
}

bool cancel(Handle h) noexcept
{
   if (h.id == 0) {
      return false;
   }

   // Same SMP note as schedule_at().
   periodic::IrqGuard guard;

   for (auto& slot : periodic::ds.slots) {
      if (slot.id == h.id) {
         slot = periodic::Slot{};
         return true;
      }
   }

   return false;
}

[[nodiscard]] Duration from_milliseconds(uint32_t ms) noexcept
{
   const uint64_t f = cortos_port_time_freq_hz();
   const uint64_t ticks = periodic::ceil_div_u64(static_cast<uint64_t>(ms) * f, 1000ULL);
   return Duration{ticks};
}

[[nodiscard]] Duration from_microseconds(uint32_t us) noexcept
{
   const uint64_t f = cortos_port_time_freq_hz();
   const uint64_t ticks = periodic::ceil_div_u64(static_cast<uint64_t>(us) * f, 1'000'000ULL);
   return Duration{ticks};
}

void start() noexcept
{
   if (periodic::ds.started) {
      return;
   }

   CORTOS_ASSERT(periodic::ds.tick_frequency_hz > 0);

   cortos_port_time_register_isr_handler(&periodic::isr_trampoline, nullptr);
   cortos_port_time_setup(periodic::ds.tick_frequency_hz);
   cortos_port_time_irq_enable();

   periodic::ds.started = true;
}

void stop() noexcept
{
   if (!periodic::ds.started) {
      return;
   }

   cortos_port_time_irq_disable();
   periodic::ds.started = false;
}

void on_timer_isr() noexcept
{
   const uint64_t now_ticks = cortos_port_time_now();
   periodic::fire_due_isr(now_ticks);
}

} // namespace cortos::time
