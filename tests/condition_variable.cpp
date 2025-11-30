#include "cornishrtk.hpp"
#include "port_traits.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

// --- Global sync primitives and bookkeeping ---------------------------------

static constinit rtk::Mutex        mutex;
static constinit rtk::ConditionVar cond_var;

static std::atomic<int> blocked_count{0};  // how many workers have reached wait()
static std::atomic<int> wake_seq{0};       // incremented as workers wake

static int order_high = -1;
static int order_mid  = -1;
static int order_low  = -1;

// Just so we can see scheduler behaviour in the logs
static void log_order()
{
   LOG_THREAD("[TEST] wake order: HIGH=%d MID=%d LOW=%d", order_high, order_mid, order_low);
}

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
   LOG_THREAD("[HIGH] started, going to wait on condition");

   mutex.lock();
   blocked_count.fetch_add(1, std::memory_order_acq_rel);

   LOG_THREAD("[HIGH] now waiting on condition");
   cond_var.wait(mutex);  // returns with mutex locked again

   int seq = wake_seq.fetch_add(1, std::memory_order_acq_rel);
   order_high = seq;
   LOG_THREAD("[HIGH] woke from wait, seq=%d", seq);

   mutex.unlock();

   while (true) rtk::Scheduler::sleep_for(1000);
}

static void worker_mid()
{
   LOG_THREAD("[MID ] started, going to wait on condition");

   mutex.lock();
   blocked_count.fetch_add(1, std::memory_order_acq_rel);

   LOG_THREAD("[MID ] now waiting on condition");
   cond_var.wait(mutex);

   int seq = wake_seq.fetch_add(1, std::memory_order_acq_rel);
   order_mid = seq;
   LOG_THREAD("[MID ] woke from wait, seq=%d", seq);

   mutex.unlock();

   while (true) rtk::Scheduler::sleep_for(1000);
}

static void worker_low()
{
   LOG_THREAD("[LOW ] started, going to wait on condition");

   mutex.lock();
   blocked_count.fetch_add(1, std::memory_order_acq_rel);

   LOG_THREAD("[LOW ] now waiting on condition");
   cond_var.wait(mutex);

   int seq = wake_seq.fetch_add(1, std::memory_order_acq_rel);
   order_low = seq;
   LOG_THREAD("[LOW ] woke from wait, seq=%d", seq);

   mutex.unlock();

   while (true) rtk::Scheduler::sleep_for(1000);
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
   LOG_THREAD("[CTRL] started");

   // Wait for all 3 workers to be blocked on the condvar.
   // Using yield() instead of busy spinning purely in CPU.
   while (blocked_count.load(std::memory_order_acquire) < 3) {
      LOG_THREAD("[CTRL] waiting for workers to block, blocked_count=%d", blocked_count.load(std::memory_order_relaxed));
      rtk::Scheduler::sleep_for(3);
   }

   LOG_THREAD("[CTRL] all workers are blocked, starting notify_one() sequence");

   // First wake: should go to highest-priority waiter (HIGH)
   LOG_THREAD("[CTRL] notify_one() #1\n");
   mutex.lock();
   cond_var.notify_one();
   mutex.unlock();

   rtk::Scheduler::sleep_for(5);

   // Second wake: next highest (MID)
   LOG_THREAD("[CTRL] notify_one() #2");
   mutex.lock();
   cond_var.notify_one();
   mutex.unlock();

   rtk::Scheduler::sleep_for(5);

   // Third wake: lowest (LOW)
   LOG_THREAD("[CTRL] notify_one() #3");
   mutex.lock();
   cond_var.notify_one();
   mutex.unlock();

   rtk::Scheduler::sleep_for(5);

   log_order();

   bool pass = order_high == 0 && order_mid == 1 && order_low == 2;

   if (pass) {
      LOG_THREAD("[CTRL] TEST PASS: condvar woke waiters in priority order.");
   } else {
      LOG_THREAD("[CTRL] TEST FAIL!\n");
      LOG_THREAD("       Expected wake order: HIGH=0 MID=1 LOW=2");
      log_order();
   }

   // Park forever
   while (true) rtk::Scheduler::sleep_for(1000);
}

// --- Stacks and threads -----------------------------------------------------

static constexpr std::size_t STACK_BYTES = 1024 * 8;

alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> controller_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> high_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> mid_stack{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> low_stack{};

int main()
{
   rtk::Scheduler::init(10);

   // Priorities: 0 is highest.
   rtk::Thread controller_thread(rtk::Thread::Entry(controller),
                                 controller_stack,
                                 rtk::Thread::Priority(0));

   rtk::Thread high_thread(rtk::Thread::Entry(worker_high),
                           high_stack,
                           rtk::Thread::Priority(1));

   rtk::Thread mid_thread(rtk::Thread::Entry(worker_mid),
                          mid_stack,
                          rtk::Thread::Priority(2));

   rtk::Thread low_thread(rtk::Thread::Entry(worker_low),
                          low_stack,
                          rtk::Thread::Priority(3));

   LOG_THREAD("[MAIN] starting scheduler");
   rtk::Scheduler::start();

   // Not reached
   return 0;
}
