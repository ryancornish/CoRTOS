#include "cortos.hpp"
#include "port_traits.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

using namespace cortos;

// --- Shared test state ------------------------------------------------------

static std::atomic<int> sequence{0};

static constinit int  fired_a = -1;
static constinit int  fired_b = -1;
static constinit int  fired_c = -1;
static constinit bool timer_d_fired = false;

// Used by the controller to wait until all three timers have fired
static constinit Semaphore done_sem{0};

// --- Timer callbacks (run on the kernel timer thread) -----------------------

static void timer_a_cb()
{
   int seq = sequence.fetch_add(1, std::memory_order_relaxed);
   fired_a = seq;
   LOG_TEST("[TIMER A] fired at seq=%d", seq);

   // After the 3rd callback (seq = 0,1,2) we wake the controller
   if (seq == 2) done_sem.release();
}

static void timer_b_cb()
{
   int seq = sequence.fetch_add(1, std::memory_order_relaxed);
   fired_b = seq;
   LOG_TEST("[TIMER B] fired at seq=%d", seq);
   if (seq == 2) done_sem.release();
}

static void timer_c_cb()
{
   int seq = sequence.fetch_add(1, std::memory_order_relaxed);
   fired_c = seq;
   LOG_TEST("[TIMER C] fired at seq=%d", seq);
   if (seq == 2) done_sem.release();
}

static void timer_d_cb()
{
   // This one *should not* fire if stop() works.
   timer_d_fired = true;
   LOG_TEST("[TIMER D] *** UNEXPECTEDLY FIRED ***");
}

// --- Controller thread ------------------------------------------------------
//
// Runs as a normal user thread.
// Sets up timers, waits for them to fire, and prints PASS/FAIL.
//

static void controller()
{
   LOG_TEST("[CTRL] started");

   // Timers live for the duration of this thread.
   Timer ta{ Timer::Callback(timer_a_cb), Timer::Mode::OneShot };
   Timer tb{ Timer::Callback(timer_b_cb), Timer::Mode::OneShot };
   Timer tc{ Timer::Callback(timer_c_cb), Timer::Mode::OneShot };
   Timer td{ Timer::Callback(timer_d_cb), Timer::Mode::OneShot };

   // Schedule with staggered delays (ticks are whatever your port defines)
   LOG_TEST("[CTRL] arming timers A,B,C,D");
   ta.start_after(5);   // earliest
   tb.start_after(10);
   tc.start_after(15);  // latest of the three

   td.start_after(20);  // should never actually fire
   td.stop();           // cancel before expiry

   // Wait until all three timers have fired once
   LOG_TEST("[CTRL] waiting for all three timers to fire...");
   done_sem.acquire();

   // Give the system a bit of extra time to 'accidentally' trigger D
   LOG_TEST("[CTRL] all three timers reported, sleeping to check td.stop()");
   Scheduler::sleep_for(30);

   // Evaluate results
   LOG_TEST("[CTRL] results:");
   LOG_TEST("       A seq = %d", fired_a);
   LOG_TEST("       B seq = %d", fired_b);
   LOG_TEST("       C seq = %d", fired_c);
   LOG_TEST("       D fired = %s", timer_d_fired ? "true" : "false");

   if (fired_a == 0 && fired_b == 1 && fired_c == 2 && !timer_d_fired) {
      LOG_TEST("[CTRL] TEST PASS: timers expired in order and stop() suppressed D.");
   } else {
      LOG_TEST("[CTRL] TEST FAIL!");
      LOG_TEST("       Expected: A=0, B=1, C=2, D=false");
   }
}

// --- Stacks and main --------------------------------------------------------

static constexpr std::size_t STACK_BYTES = 4096 * 4;

alignas(CORTOS_STACK_ALIGN) static std::array<std::byte, STACK_BYTES> controller_stack{};

int main()
{
   Scheduler::init(10);

   // Controller runs at a non-zero priority (timer thread is priority 0)
   Thread controller_thread(
      Thread::Entry(controller),
      controller_stack,
      Thread::Priority(1)
   );

   LOG_TEST("[MAIN] starting scheduler");
   Scheduler::start();

   return 0;
}
