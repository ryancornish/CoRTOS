#include "cortos.hpp"
#include "port_traits.h"

#include <iostream>
#include <cstdint>
#include <array>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

static constexpr std::size_t STACK_BYTES = 1024 * 16;
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> t1_stack{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> t2_stack{};

static void worker(const char* name)
{
   while (true)
   {
      LOG_THREAD("%s", name);
      cortos::Scheduler::sleep_for(20); // Tune for testing
   }
}

int main()
{
   cortos::Scheduler::init(10); // Tune for testing

   // Equal priority -> round-robin
   cortos::Thread t1(cortos::Thread::Entry([]{worker("T1 tick");}), t1_stack, cortos::Thread::Priority(2));
   cortos::Thread t2(cortos::Thread::Entry([]{worker("T2 tock");}), t2_stack, cortos::Thread::Priority(2));

   // In simulation, start() returns into an internal loop and never exits main.
   // On MCU ports, start() will not return (noreturn path).
   cortos::Scheduler::start();
   return 0;
}
