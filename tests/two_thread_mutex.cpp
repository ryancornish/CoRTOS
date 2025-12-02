#include "cornishrtk.hpp"
#include "port_traits.h"

#include <array>
#include <cstdint>
#include <iostream>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

static constexpr std::size_t STACK_BYTES = 1024 * 8;
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> t1_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> t2_stack{};

// Shared state
constinit static rtk::Mutex mutex;
static std::uint32_t counter = 0;

// Slightly different sleep periods so we see interleaving
constexpr std::uint32_t T1_SLEEP_TICKS = 15;
constexpr std::uint32_t T2_SLEEP_TICKS = 20;

static void worker(char const* name)
{
   while (true) {
      // --- Critical section ---
      mutex.lock();
      auto my_id = rtk::Scheduler::tick_now().value(); // just to show something
      std::uint32_t local = ++counter;

      LOG_THREAD("[%s] acquired mutex. Counter=%u, tick=%u", name, local, my_id);

      // Simulate some work while holding the lock
      rtk::Scheduler::sleep_for(5);

      LOG_THREAD("[%s] releasing mutex. Counter=%u", name, local);
      mutex.unlock();
      // --- End critical section ---

      // Now sleep outside the lock so the other thread gets a turn.
      if (std::string_view{name} == "T1") {
         rtk::Scheduler::sleep_for(T1_SLEEP_TICKS);
      } else {
         rtk::Scheduler::sleep_for(T2_SLEEP_TICKS);
      }
   }
}

int main()
{
   rtk::Scheduler::init(5);

   rtk::Thread t1(rtk::Thread::Entry([]{worker("T1  ");}), t1_stack, rtk::Thread::Priority(1));

   rtk::Thread t2(rtk::Thread::Entry([]{worker("T2  ");}), t2_stack, rtk::Thread::Priority(2));

   rtk::Scheduler::start();
   return 0;
}
