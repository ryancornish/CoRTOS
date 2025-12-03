#include "cortos.hpp"
#include "port_traits.h"

#include <array>
#include <cstdint>
#include <iostream>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

static constexpr std::size_t STACK_BYTES = 1024 * 4;
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> fast_stack{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> slow_stack{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> logger_stack{};

static void fast_worker(const char* name)
{
   std::uint32_t counter = 0;

   while (true)
   {
      LOG_THREAD("[FAST ] %s count=%u", name, counter);
      // Runs fairly often. high priority so it preempts others.
      cortos::Scheduler::sleep_for(10);
   }
}

static void slow_worker(const char* name)
{
   std::uint32_t counter = 0;

   while (true)
   {
      LOG_THREAD("[SLOW ] %s count=%u", name, counter++);
      cortos::Scheduler::sleep_for(25);
   }
}

static void logger_worker()
{
   std::uint32_t tick_block = 0;

   while (true)
   {
      auto now = cortos::Scheduler::tick_now().value();
      LOG_THREAD("[LOG ] heartbeat at tick=%u (block=%u)", now, tick_block++);

      // Sleep long enough that higher-priority work dominates,
      // but occasionally yield cooperatively to show both paths.
      cortos::Scheduler::sleep_for(50);

      // Explicit cooperative yield inside our own timeslice
      // (on real MCU this would just pend a switch).
      cortos::Scheduler::yield();
   }
}

int main()
{
   cortos::Scheduler::init(10);

   // Priorities: lower number = higher priority.
   //   fast:   prio 1 (preempts others)
   //   slow:   prio 2
   //   logger: prio 10 (only runs when others are sleeping)
   cortos::Thread fast_thread(cortos::Thread::Entry([]{fast_worker("fast_worker");}), fast_stack, cortos::Thread::Priority(1));

   cortos::Thread slow_thread(cortos::Thread::Entry([]{slow_worker("slow_worker");}), slow_stack, cortos::Thread::Priority(2));

   cortos::Thread logger_thread(cortos::Thread::Entry(logger_worker), logger_stack, cortos::Thread::Priority(10));

   cortos::Scheduler::start();
   return 0;
}
