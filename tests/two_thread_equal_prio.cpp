#include "cornishrtk.hpp"
#include "port_traits.h"

#include <iostream>
#include <cstdint>
#include <array>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

static constexpr std::size_t STACK_BYTES = 1024 * 16;
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> t1_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> t2_stack{};

static void worker(const char* name)
{
   while (true)
   {
      LOG_THREAD("%s", name);
      rtk::Scheduler::sleep_for(20); // Tune for testing
   }
}

int main()
{
   rtk::Scheduler::init(10); // Tune for testing

   // Equal priority -> round-robin
   rtk::Thread t1(rtk::Thread::Entry([]{worker("T1 tick");}), t1_stack, rtk::Thread::Priority(2));
   rtk::Thread t2(rtk::Thread::Entry([]{worker("T2 tock");}), t2_stack, rtk::Thread::Priority(2));

   // In simulation, start() returns into an internal loop and never exits main.
   // On MCU ports, start() will not return (noreturn path).
   rtk::Scheduler::start();
   return 0;
}
