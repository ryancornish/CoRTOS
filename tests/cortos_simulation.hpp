#ifndef CORTOS_SIMULATION_HPP
#define CORTOS_SIMULATION_HPP

namespace cortos::sim
{
   void run_forever(unsigned tick_hz);
   void advance_tick(int ticks = 1);
}

#endif
