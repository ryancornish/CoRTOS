#ifndef CORTOS_SIMULATION_HPP
#define CORTOS_SIMULATION_HPP

#include <cstdint>

namespace cortos::sim
{
   void run_for(unsigned ticks);
   void run_forever();
   void run_until_quiescent();
   void advance_tick(unsigned ticks = 1);
   uint32_t get_tick_hz();
}

#endif
