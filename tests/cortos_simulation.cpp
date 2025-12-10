#include "cortos_simulation.hpp"
#include "cortos.hpp"
#include <ctime>

namespace cortos::sim
{
   // Definition provided by the kernel when CORTOS_SIMULATION == 1
   // Allows direct rescheduling
   void schedule_under_sim();


   void run_forever(unsigned tick_hz)
   {
      auto const ns_per_tick = 1'000'000'000ull / tick_hz;
      while (true) {
         advance_tick();  // one tick worth of work
         schedule_under_sim();

         struct timespec req {
            .tv_sec  = 0,
            .tv_nsec = static_cast<long>(ns_per_tick)
         };
         nanosleep(&req, nullptr);
      }
   }

}