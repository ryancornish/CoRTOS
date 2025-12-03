#include "cortos.hpp"
#include "port_traits.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

// --- Global semaphore and bookkeeping ---------------------------------------
static constinit rtk::Semaphore sem{0};
static constinit std::atomic<int> acquire_seq{0};
static constinit int order_high = -1;
static constinit int order_mid  = -1;
static constinit int order_low  = -1;

// --- Worker threads ---------------------------------------------------------

static void worker_high()
{
   LOG_TEST("[HIGH] started, calling acquire()");
   sem.acquire();
   int seq = acquire_seq.fetch_add(1, std::memory_order_relaxed);
   order_high = seq;
   LOG_TEST("[HIGH] acquired semaphore at seq=%d", seq);
}

static void worker_mid()
{
   LOG_TEST("[MID ] started, calling acquire()");
   sem.acquire();
   int seq = acquire_seq.fetch_add(1, std::memory_order_relaxed);
   order_mid = seq;
   LOG_TEST("[MID ] acquired semaphore at seq=%d", seq);
}

static void worker_low()
{
   LOG_TEST("[LOW ] started, calling acquire()");
   sem.acquire();
   int seq = acquire_seq.fetch_add(1, std::memory_order_relaxed);
   order_low = seq;
   LOG_TEST("[LOW ] acquired semaphore at seq=%d", seq);
}

// --- Controller thread ------------------------------------------------------
//
// Priority 0 (highest) so it can orchestrate the releases deterministically.
//

static void controller()
{
   LOG_TEST("[CTRL] started\n");

   // Give workers a chance to start and block on sem.acquire().
   // (If they haven't blocked yet, they'll just consume tokens earlier, which
   //  is still fine as long as they *eventually* all block before the last release.)
   rtk::Scheduler::sleep_for(5);

   LOG_TEST("[CTRL] releasing 1 token");
   sem.release(1);
   rtk::Scheduler::sleep_for(5);

   LOG_TEST("[CTRL] releasing 1 token");
   sem.release(1);
   rtk::Scheduler::sleep_for(5);

   LOG_TEST("[CTRL] releasing 1 token");
   sem.release(1);
   rtk::Scheduler::sleep_for(5);

   bool pass = order_high == 0 && order_mid  == 1 && order_low  == 2;

   if (pass) {
      LOG_TEST("[CTRL] TEST PASS: semaphore woke waiters in priority order.");
   } else {
      LOG_TEST("[CTRL] TEST FAIL!");
      LOG_TEST("       Expected: HIGH=0 MID=1 LOW=2");
      LOG_TEST("         Actual: HIGH=%d MID=%d LOW=%d", order_high, order_mid, order_low);
   }
}

// --- Snacks and threads -----------------------------------------------------
static constexpr std::size_t STACK_BYTES = 1024 * 4;
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

   LOG_TEST("[MAIN] starting scheduler");
   rtk::Scheduler::start();

   // Not reached
   return 0;
}
