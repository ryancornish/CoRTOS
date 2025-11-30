#include "cornishrtk.hpp"
#include "port_traits.h"

#include <array>
#include <cstdint>
#include <iostream>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

static constinit rtk::Mutex mutex;
static constinit std::uint32_t shared_counter = 0;

static constexpr std::size_t STACK_BYTES = 1024 * 16;
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> timed1_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> timed2_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> blocking_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> monitor_stack{};

// High-priority timed worker (uses try_lock_for / try_lock_until)
static void timed_worker(void* arg)
{
   auto const* name = static_cast<char const*>(arg);
   std::uint32_t iteration = 0;

   while (true) {
      auto now = rtk::Scheduler::tick_now().value();

      LOG_TEST("[%s] @ITER(%u) trying timed lock (5 ticks)", name, iteration);

      auto const deadline = rtk::Scheduler::tick_now() + 5;
      if (mutex.try_lock_until(deadline)) {
         now = rtk::Scheduler::tick_now().value();
         ++shared_counter;

         LOG_TEST("[%s] acquired mutex. @SHRD_CTR(%u)", name, shared_counter);

         // Hold it a bit, enough to make the other timed worker occasionally time out
         rtk::Scheduler::sleep_for(7);

         now = rtk::Scheduler::tick_now().value();

         LOG_TEST("[%s] releasing mutex", name);

         mutex.unlock();
      } else {
         now = rtk::Scheduler::tick_now().value();

         LOG_TEST("[%s] timed out waiting for mutex", name);
      }

      // Back off so both timed workers get a chance to run
      rtk::Scheduler::sleep_for(10);
      ++iteration;
   }
}

// Medium-priority worker that uses *blocking* Mutex::lock()
static void blocking_worker(void* arg)
{
   char const* name = static_cast<char const*>(arg);
   std::uint32_t iteration = 0;

   while (true) {
      auto now = rtk::Scheduler::tick_now().value();

      LOG_TEST("[%s] @ITER(%u) attempting to lock() mutex", name, iteration);

      // This exercises the waiter queue:
      // - If the mutex is free: immediate acquisition, no waiters.
      // - If held: this thread becomes Blocked and is enqueued
      //   in the mutex wait list, to be woken by Mutex::unlock().
      mutex.lock();

      now = rtk::Scheduler::tick_now().value();
      ++shared_counter;

      LOG_TEST("[%s] acquired mutex. @SHRD_CTR(%u)", name, shared_counter);

      // Hold a while so:
      // - timed workers may time out,
      // - we clearly see the hand-off after unlock.
      rtk::Scheduler::sleep_for(6);

      now = rtk::Scheduler::tick_now().value();

      LOG_TEST("[%s] releasing mutex", name);

      mutex.unlock();

      // Back off for a bit
      rtk::Scheduler::sleep_for(9);
      ++iteration;
   }
}

// Low-priority monitor
static void monitor_worker(void* arg)
{
   char const* name = static_cast<char const*>(arg);
   std::uint32_t heartbeat = 0;

   while (true) {
      auto now = rtk::Scheduler::tick_now().value();
      bool locked = mutex.is_locked();

      LOG_TEST("[%s] @HEARTBEAT(%u) @SHRD_CTR(%u)", name, heartbeat, shared_counter);
      LOG_TEST("[%s] mutex is %s", name, locked ? "busy, cannot acquire" : "free, acquirable" );

      ++heartbeat;
      rtk::Scheduler::sleep_for(20);
   }
}

int main()
{
   // 10 ticks per second in this simulation
   rtk::Scheduler::init(10);

   // Two timed-lock workers at highest user priority (1)
   rtk::Thread timed_worker1(
      rtk::Thread::Entry(timed_worker, (void*)"T1  "),
      timed1_stack,
      rtk::Thread::Priority(1));

   rtk::Thread timed_worker2(
      rtk::Thread::Entry(timed_worker, (void*)"T2  "),
      timed2_stack,
      rtk::Thread::Priority(1));

   // Blocking lock() user at slightly *lower* priority (2).
   // This lets T1/T2 sometimes own the mutex while BLOCK blocks and
   // enters the waiter queue, but also lets BLOCK get the CPU often enough
   // to demonstrate both immediate and queued acquisition.
   rtk::Thread blocking_thread(
      rtk::Thread::Entry(blocking_worker, (void*)"BLOCK"),
      blocking_stack,
      rtk::Thread::Priority(2));

   // Monitor at low priority (10)
   rtk::Thread monitor_thread(
      rtk::Thread::Entry(monitor_worker, (void*)"MON"),
      monitor_stack,
      rtk::Thread::Priority(10));

   rtk::Scheduler::start();
   return 0;
}
