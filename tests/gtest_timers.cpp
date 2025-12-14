#include "cortos.hpp"
#include "cortos_simulation.hpp"
#include "port_traits.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <gtest/gtest.h>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"


static class TimersTestFixture* current_fixture = nullptr;
class TimersTestFixture : public ::testing::Test
{
public:
   // Used by the controller to wait until all three timers have fired
   cortos::Semaphore done_sem{0};
   std::atomic<int> sequence{0};
   int  fired_a = -1;
   int  fired_b = -1;
   int  fired_c = -1;
   bool timer_d_fired = false;
protected:

   void SetUp() override
   {
      current_fixture = this;
      cortos::Scheduler::init(10);
   }

   void TearDown() override
   {
      cortos::Scheduler::kill_idle_thread();
      cortos::Scheduler::kill_timer_thread();
      current_fixture = nullptr;
   }
};


// --- Timer callbacks (run on the kernel timer thread) -----------------------

static void timer_a_cb()
{
   int seq = current_fixture->sequence.fetch_add(1, std::memory_order_relaxed);
   current_fixture->fired_a = seq;
   LOG_TEST("[TIMER A] fired at seq=%d", seq);

   // After the 3rd callback (seq = 0,1,2) we wake the controller
   if (seq == 2) current_fixture->done_sem.release();
}

static void timer_b_cb()
{
   int seq = current_fixture->sequence.fetch_add(1, std::memory_order_relaxed);
   current_fixture->fired_b = seq;
   LOG_TEST("[TIMER B] fired at seq=%d", seq);
   if (seq == 2) current_fixture->done_sem.release();
}

static void timer_c_cb()
{
   int seq = current_fixture->sequence.fetch_add(1, std::memory_order_relaxed);
   current_fixture->fired_c = seq;
   LOG_TEST("[TIMER C] fired at seq=%d", seq);
   if (seq == 2) current_fixture->done_sem.release();
}

static void timer_d_cb()
{
   // This one *should not* fire if stop() works.
   current_fixture->timer_d_fired = true;
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
   cortos::Timer ta{ cortos::Timer::Callback(timer_a_cb), cortos::Timer::Mode::OneShot };
   cortos::Timer tb{ cortos::Timer::Callback(timer_b_cb), cortos::Timer::Mode::OneShot };
   cortos::Timer tc{ cortos::Timer::Callback(timer_c_cb), cortos::Timer::Mode::OneShot };
   cortos::Timer td{ cortos::Timer::Callback(timer_d_cb), cortos::Timer::Mode::OneShot };

   // Schedule with staggered delays (ticks are whatever your port defines)
   LOG_TEST("[CTRL] arming timers A,B,C,D");
   ta.start_after(5);   // earliest
   tb.start_after(10);
   tc.start_after(15);  // latest of the three

   td.start_after(20);  // should never actually fire
   td.stop();           // cancel before expiry

   // Wait until all three timers have fired once
   LOG_TEST("[CTRL] waiting for all three timers to fire...");
   current_fixture->done_sem.acquire();

   // Give the system a bit of extra time to 'accidentally' trigger D
   LOG_TEST("[CTRL] all three timers reported, sleeping to check td.stop()");
   cortos::Scheduler::sleep_for(30);
}

static constexpr std::size_t STACK_BYTES = 4096 * 4;

alignas(CORTOS_STACK_ALIGN) static std::array<std::byte, STACK_BYTES> controller_stack{};

TEST_F(TimersTestFixture, TimersExpireInOrder)
{
   // Controller runs at a non-zero priority (timer thread is priority 0)
   cortos::Thread controller_thread(
      cortos::Thread::Entry(controller),
      controller_stack,
      cortos::Thread::Priority(1)
   );

   LOG_TEST("[MAIN] starting scheduler");
   cortos::Scheduler::start();

   //cortos::sim::run_for(55);
   cortos::sim::run_until_quiescent();

   EXPECT_EQ(fired_a, 0) << "Timer A should fire first!";
   EXPECT_EQ(fired_b, 1) << "Timer B should fire second!";
   EXPECT_EQ(fired_c, 2) << "Timer C should fire third!";
   EXPECT_FALSE(timer_d_fired) << "Timer D was cancelled and should not have fired!";

}
