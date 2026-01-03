

#include "cortos/kernel.hpp"
#include "cortos/port.h"
namespace cortos
{
void Spinlock::lock()
{
   while (flag.test_and_set(std::memory_order_acquire)) {
      // Busy-wait with CPU yield hint
      cortos_port_cpu_relax();
   }
}
}
