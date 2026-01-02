#include "cortos_simulation.hpp"
#include "cortos.hpp"
#include <ctime>

namespace cortos::sim
{
   // Definition provided by the kernel when CORTOS_SIMULATION == 1
   void schedule_under_sim();  // Allows direct rescheduling
   bool system_is_quiescent(); // Peeks internal scheduler state for quiescence


   static void tick_period_sleep()
   {
      auto tick_hz = get_tick_hz();
      if (tick_hz == 1) {
         struct timespec req {
            .tv_sec  = 1,
            .tv_nsec = 0
         };
         nanosleep(&req, nullptr);
      } else {
         auto const ns_per_tick = 1'000'000'000ull / tick_hz;
         struct timespec req {
            .tv_sec  = 0,
            .tv_nsec = static_cast<long>(ns_per_tick)
         };
         nanosleep(&req, nullptr);
      }
   }

   void step_tick()
   {
      advance_tick();
      schedule_under_sim();
   }

   void run_for(unsigned ticks)
   {
      for (unsigned i = 0; i < ticks; i++) {
         step_tick();
         tick_period_sleep();
      }
   }

   void run_forever()
   {
      while (true) {
         step_tick();
         tick_period_sleep();
      }
   }

   void run_until_quiescent()
   {
      while (true) {
         step_tick();

         if (system_is_quiescent()) return;
         tick_period_sleep();
      }
   }

}