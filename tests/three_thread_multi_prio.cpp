#include "cornishrtk.hpp"
#include "port_traits.h"

#include <array>
#include <cstdint>
#include <iostream>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

static constexpr std::size_t STACK_BYTES = 1024 * 4;
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> fast_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> slow_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> logger_stack{};

static void fast_worker(void* arg)
{
   const char* name = static_cast<const char*>(arg);
   std::uint32_t counter = 0;

   while (true)
   {
      LOG_THREAD("[FAST ] %s count=%u", name, counter);
      // Runs fairly often. high priority so it preempts others.
      rtk::Scheduler::sleep_for(10);
   }
}

static void slow_worker(void* arg)
{
   const char* name = static_cast<const char*>(arg);
   std::uint32_t counter = 0;

   while (true)
   {
      LOG_THREAD("[SLOW ] %s count=%u", name, counter++);
      rtk::Scheduler::sleep_for(25);
   }
}

static void logger_worker()
{
   std::uint32_t tick_block = 0;

   while (true)
   {
      auto now = rtk::Scheduler::tick_now().value();
      LOG_THREAD("[LOG ] heartbeat at tick=%u (block=%u)", now, tick_block++);

      // Sleep long enough that higher-priority work dominates,
      // but occasionally yield cooperatively to show both paths.
      rtk::Scheduler::sleep_for(50);

      // Explicit cooperative yield inside our own timeslice
      // (on real MCU this would just pend a switch).
      rtk::Scheduler::yield();
   }
}

int main()
{
   rtk::Scheduler::init(10);

   // Priorities: lower number = higher priority.
   //   fast:   prio 1 (preempts others)
   //   slow:   prio 2
   //   logger: prio 10 (only runs when others are sleeping)
   rtk::Thread fast_thread(rtk::Thread::Entry(fast_worker, (void*)"fast_worker"), fast_stack, rtk::Thread::Priority(1));

   rtk::Thread slow_thread(rtk::Thread::Entry(slow_worker, (void*)"slow_worker"), slow_stack, rtk::Thread::Priority(2));

   rtk::Thread logger_thread(rtk::Thread::Entry(logger_worker), logger_stack, rtk::Thread::Priority(10));

   rtk::Scheduler::start();
   return 0;
}
