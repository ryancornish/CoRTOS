#include "cortos.hpp"
#include "cortos_simulation.hpp"
#include "port_traits.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <gtest/gtest.h>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

// --- Global sync primitives and bookkeeping ---------------------------------

static constinit cortos::Mutex        mutex;
static constinit cortos::ConditionVar cond_var;

static std::atomic<int> blocked_count{0};  // how many workers have reached wait()
static std::atomic<int> wake_seq{0};       // incremented as workers wake

static int order_high = -1;
static int order_mid  = -1;
static int order_low  = -1;


// --- Worker threads ---------------------------------------------------------
//
// Priorities (0 is highest overall):
//   HIGH = 1
//   MID  = 2
//   LOW  = 3
//
// All three:
//  - lock the mutex
//  - increment blocked_count
//  - wait on the condvar
//  - when woken, record the sequence in which they woke
//  - unlock and park forever
//

static void worker_high()
{
   LOG_TEST("[HIGH] started, going to wait on condition");

   mutex.lock();
   blocked_count.fetch_add(1, std::memory_order_acq_rel);

   LOG_TEST("[HIGH] now waiting on condition");
   cond_var.wait(mutex);  // returns with mutex locked again

   int seq = wake_seq.fetch_add(1, std::memory_order_acq_rel);
   order_high = seq;
   LOG_TEST("[HIGH] woke from wait, seq=%d", seq);

   mutex.unlock();

   LOG_TEST("[HIGH] ended");
}

static void worker_mid()
{
   LOG_TEST("[MID ] started, going to wait on condition");

   mutex.lock();
   blocked_count.fetch_add(1, std::memory_order_acq_rel);

   LOG_TEST("[MID ] now waiting on condition");
   cond_var.wait(mutex);

   int seq = wake_seq.fetch_add(1, std::memory_order_acq_rel);
   order_mid = seq;
   LOG_TEST("[MID ] woke from wait, seq=%d", seq);

   mutex.unlock();

   LOG_TEST("[MID ] ended");
}

static void worker_low()
{
   LOG_TEST("[LOW ] started, going to wait on condition");

   mutex.lock();
   blocked_count.fetch_add(1, std::memory_order_acq_rel);

   LOG_TEST("[LOW ] now waiting on condition");
   cond_var.wait(mutex);

   int seq = wake_seq.fetch_add(1, std::memory_order_acq_rel);
   order_low = seq;
   LOG_TEST("[LOW ] woke from wait, seq=%d", seq);

   mutex.unlock();

   LOG_TEST("[LOW ] ended");
}

// --- Controller thread ------------------------------------------------------
//
// Priority 0 (highest) so it can orchestrate deterministically.
//
// 1. Wait until all three workers have called wait() and are blocked.
// 2. Call notify_one() three times, with sleeps between, so we can observe
//    wake order clearly.
// 3. Check that wake order is HIGH=0, MID=1, LOW=2.
//

static void controller()
{
   LOG_TEST("[CTRL] started");

   // Wait for all 3 workers to be blocked on the condvar.
   // Using yield() instead of busy spinning purely in CPU.
   while (blocked_count.load(std::memory_order_acquire) < 3) {
      LOG_TEST("[CTRL] waiting for workers to block, blocked_count=%d", blocked_count.load(std::memory_order_relaxed));
      cortos::Scheduler::sleep_for(3);
   }

   LOG_TEST("[CTRL] all workers are blocked, starting notify_one() sequence");

   // First wake: should go to highest-priority waiter (HIGH)
   LOG_TEST("[CTRL] notify_one() #1");
   mutex.lock();
   cond_var.notify_one();
   mutex.unlock();

   cortos::Scheduler::sleep_for(5);

   // Second wake: next highest (MID)
   LOG_TEST("[CTRL] notify_one() #2");
   mutex.lock();
   cond_var.notify_one();
   mutex.unlock();

   cortos::Scheduler::sleep_for(5);

   // Third wake: lowest (LOW)
   LOG_TEST("[CTRL] notify_one() #3");
   mutex.lock();
   cond_var.notify_one();
   mutex.unlock();

   cortos::Scheduler::sleep_for(5);

   LOG_TEST("[CTRL] ended");
}

// --- Snacks and threads -----------------------------------------------------
static constexpr std::size_t STACK_BYTES = 1024 * 8;

alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> controller_stack{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> high_stack{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> mid_stack{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> low_stack{};


class ConditionVarTestFixture : public ::testing::Test
{
protected:
   void SetUp() override
   {
      blocked_count.store(0, std::memory_order_relaxed);
      wake_seq.store(0, std::memory_order_relaxed);
      order_high = order_mid = order_low = -1;

      cortos::Scheduler::init(10);
   }

   void TearDown() override
   {
      cortos::Scheduler::kill_idle_thread();
      cortos::Scheduler::kill_timer_thread();
   }
};

TEST_F(ConditionVarTestFixture, WaitersWakeInPriorityOrder)
{
   // Create threads
   cortos::Thread ctrl_thread(cortos::Thread::Entry(controller), controller_stack, cortos::Thread::Priority(0));

   cortos::Thread high_thread(cortos::Thread::Entry(worker_high), high_stack, cortos::Thread::Priority(1));

   cortos::Thread mid_thread(cortos::Thread::Entry(worker_mid), mid_stack, cortos::Thread::Priority(2));

   cortos::Thread low_thread(cortos::Thread::Entry(worker_low), low_stack, cortos::Thread::Priority(3));

   LOG_TEST("[TEST] starting scheduler");
   cortos::Scheduler::start();


   //cortos::sim::run_for(50);
   cortos::sim::run_until_quiescent();

   EXPECT_EQ(order_high, 0) << "High-priority waiter should wake first";
   EXPECT_EQ(order_mid,  1) << "Mid-priority waiter should wake second";
   EXPECT_EQ(order_low,  2) << "Low-priority waiter should wake third";
}

