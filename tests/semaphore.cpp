#include "cornishrtk.hpp"
#include "port_traits.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

// --- Global semaphore and bookkeeping ---------------------------------------

static constinit rtk::Semaphore sem{0};

static std::atomic<int> acquire_seq{0};
static int order_high = -1;
static int order_mid  = -1;
static int order_low  = -1;

// Just so we can see scheduler behaviour in the logs
static void log_order()
{
   std::printf("[TEST] acquire order: HIGH=%d MID=%d LOW=%d\n", order_high, order_mid, order_low);
}

// --- Worker threads ---------------------------------------------------------

static void worker_high()
{
   std::printf("[HIGH] started, calling acquire()\n");
   sem.acquire();
   int seq = acquire_seq.fetch_add(1, std::memory_order_relaxed);
   order_high = seq;
   std::printf("[HIGH] acquired semaphore at seq=%d\n", seq);

   // Park forever so we don't re-enter the test logic
   while (true) rtk::Scheduler::yield();
}

static void worker_mid()
{
   std::printf("[MID ] started, calling acquire()\n");
   sem.acquire();
   int seq = acquire_seq.fetch_add(1, std::memory_order_relaxed);
   order_mid = seq;
   std::printf("[MID ] acquired semaphore at seq=%d\n", seq);

   while (true) rtk::Scheduler::yield();
}

static void worker_low()
{
   std::printf("[LOW ] started, calling acquire()\n");
   sem.acquire();
   int seq = acquire_seq.fetch_add(1, std::memory_order_relaxed);
   order_low = seq;
   std::printf("[LOW ] acquired semaphore at seq=%d\n", seq);

   while (true) rtk::Scheduler::yield();
}

// --- Controller thread ------------------------------------------------------
//
// Priority 0 (highest) so it can orchestrate the releases deterministically.
//

static void controller()
{
   std::printf("[CTRL] started\n");

   // Give workers a chance to start and block on sem.acquire().
   // (If they haven't blocked yet, they'll just consume tokens earlier, which
   //  is still fine as long as they *eventually* all block before the last release.)
   rtk::Scheduler::sleep_for(5);

   std::printf("[CTRL] releasing 1 token\n");
   sem.release(1);
   rtk::Scheduler::sleep_for(5);

   std::printf("[CTRL] releasing 1 token\n");
   sem.release(1);
   rtk::Scheduler::sleep_for(5);

   std::printf("[CTRL] releasing 1 token\n");
   sem.release(1);
   rtk::Scheduler::sleep_for(5);

   log_order();

   bool pass =
      (order_high == 0) &&
      (order_mid  == 1) &&
      (order_low  == 2);

   if (pass) {
      std::printf("[CTRL] TEST PASS: semaphore woke waiters in priority order.\n");
   } else {
      std::printf("[CTRL] TEST FAIL!\n");
      std::printf("       Expected: HIGH=0 MID=1 LOW=2\n");
      log_order();
   }

   while (true) rtk::Scheduler::yield();
}

// --- Stacks and threads -----------------------------------------------------
static constexpr std::size_t STACK_BYTES = 4096;
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> controller_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> high_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> mid_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> low_stack{};

int main()
{
   rtk::Scheduler::init(10);

   // Priorities: 0 is highest. Give controller the absolute highest.
   rtk::Thread controller_thread( rtk::Thread::Entry(controller), controller_stack, rtk::Thread::Priority(0));
   rtk::Thread high_thread(rtk::Thread::Entry(worker_high), high_stack, rtk::Thread::Priority(1));
   rtk::Thread mid_thread(rtk::Thread::Entry(worker_mid), mid_stack, rtk::Thread::Priority(2));
   rtk::Thread low_thread(rtk::Thread::Entry(worker_low), low_stack, rtk::Thread::Priority(3));

   std::printf("[MAIN] starting scheduler\n");
   rtk::Scheduler::start();

   // Not reached
   return 0;
}
