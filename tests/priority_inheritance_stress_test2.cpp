#include <cornishrtk.hpp>
#include <port_traits.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <format>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

// ---------------------------------------------------------------------------
// Mutexes
// ---------------------------------------------------------------------------

static constinit rtk::Mutex mutex_A; // For PI chain: MED <-> HIGH
static constinit rtk::Mutex mutex_B; // For PI chain: LOW  <-> MED
static constinit rtk::Mutex mutex_C; // For multi-waiter + timeout test

// ---------------------------------------------------------------------------
// Stacks
// ---------------------------------------------------------------------------

static constexpr std::size_t STACK_BYTES = 4096;

alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_LOW{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_MED{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_HIGH{};

alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_OWNC{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_W1{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_W2{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_W3{};

alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_MON{}; // monitor

// ---------------------------------------------------------------------------
// Thread entries - Priority Inheritance Chain (LOW, MED, HIGH)
// ---------------------------------------------------------------------------

// Low priority: holds B for a long time.
static void thread_LOW_entry()
{
   LOG_THREAD("[LOW ] enter");

   // Let others get created and maybe arrange their sleeps
   rtk::Scheduler::sleep_for(1);

   LOG_THREAD("[LOW ] locking mutex_B");
   mutex_B.lock();
   LOG_THREAD("[LOW ] acquired mutex_B, holding for 50 ticks");

   rtk::Scheduler::sleep_for(50);

   LOG_THREAD("[LOW ] unlocking mutex_B");
   mutex_B.unlock();

   LOG_THREAD("[LOW ] finish. Parking");
   while (true) {
      rtk::Scheduler::sleep_for(100);
   }
}

// Medium priority: lock A, then later try to lock B (blocked by LOW)
static void thread_MED_entry()
{
   LOG_THREAD("[MED ] enter");

   // Give LOW time to start and grab B first
   rtk::Scheduler::sleep_for(5);

   LOG_THREAD("[MED ] locking mutex_A");
   mutex_A.lock();
   LOG_THREAD("[MED ] acquired mutex_A, holding for 5 ticks");

   rtk::Scheduler::sleep_for(5);

   LOG_THREAD("[MED ] trying to lock mutex_B (should block, LOW holds it)");
   mutex_B.lock();
   LOG_THREAD("[MED ] acquired mutex_B after blocking");

   rtk::Scheduler::sleep_for(5);

   LOG_THREAD("[MED ] unlocking mutex_B");
   mutex_B.unlock();

   LOG_THREAD("[MED ] unlocking mutex_A");
   mutex_A.unlock();

   LOG_THREAD("[MED ] finish. Parking");
   while (true) {
      rtk::Scheduler::sleep_for(100);
   }
}

// High priority: eventually tries to lock A (owned by MED)
static void thread_HIGH_entry()
{
   LOG_THREAD("[HIGH] enter");

   // Let MED first lock A, and LOW already have B
   rtk::Scheduler::sleep_for(20);

   LOG_THREAD("[HIGH] trying to lock mutex_A (will block on MED, which is blocked on LOW)");
   mutex_A.lock();
   LOG_THREAD("[HIGH] acquired mutex_A after PI chain resolved");

   rtk::Scheduler::sleep_for(5);

   LOG_THREAD("[HIGH] unlocking mutex_A");
   mutex_A.unlock();

   LOG_THREAD("[HIGH] finish. Parking");
   while (true) {
      rtk::Scheduler::sleep_for(100);
   }
}

// ---------------------------------------------------------------------------
// Thread entries - Multi-waiter + timeout on mutex_C
// ---------------------------------------------------------------------------

// Owner of mutex_C: low-ish priority, holds it for a while
static void thread_OWNER_C_entry()
{
   LOG_THREAD("[OWNC] enter");

   LOG_THREAD("[OWNC] locking mutex_C");
   mutex_C.lock();
   LOG_THREAD("[OWNC] acquired mutex_C, holding for 40 ticks");

   rtk::Scheduler::sleep_for(40);

   LOG_THREAD("[OWNC] unlocking mutex_C");
   mutex_C.unlock();

   LOG_THREAD("[OWNC] finish. Parking");
   while (true) {
      rtk::Scheduler::sleep_for(100);
   }
}

// High-priority waiter with a *short* timeout: donates briefly, then times out
static void thread_W1_entry()
{
   LOG_THREAD("[W1  ] enter");

   // Wait a bit so OWNER_C has mutex_C
   rtk::Scheduler::sleep_for(5);

   LOG_THREAD("[W1  ] try_lock_for(5) on mutex_C (should TIMEOUT while donating)");
   bool got = mutex_C.try_lock_for(5);
   if (got) {
      LOG_THREAD("[W1  ] UNEXPECTEDLY acquired mutex_C");
      mutex_C.unlock();
   } else {
      LOG_THREAD("[W1  ] timed out on mutex_C, no longer waiting");
   }

   LOG_THREAD("[W1  ] finish. Parking");
   while (true) {
      rtk::Scheduler::sleep_for(100);
   }
}

// Medium-high waiter with a long timeout: should eventually acquire mutex_C
static void thread_W2_entry()
{
   LOG_THREAD("[W2  ] enter");

   // Arrive shortly after W1
   rtk::Scheduler::sleep_for(6);

   LOG_THREAD("[W2  ] try_lock_for(50) on mutex_C (should acquire after OWNER_C unlocks)");
   bool got = mutex_C.try_lock_for(50);
   if (got) {
      LOG_THREAD("[W2  ] acquired mutex_C after waiting");
      rtk::Scheduler::sleep_for(5);
      LOG_THREAD("[W2  ] unlocking mutex_C");
      mutex_C.unlock();
   } else {
      LOG_THREAD("[W2  ] UNEXPECTED timeout on mutex_C");
   }

   LOG_THREAD("[W2  ] finish. Parking");
   while (true) {
      rtk::Scheduler::sleep_for(100);
   }
}

// Lower-priority waiter: plain lock, should be last to acquire mutex_C
static void thread_W3_entry()
{
   LOG_THREAD("[W3  ] enter");

   // Arrive after W2 - still before OWNER_C unlocks
   rtk::Scheduler::sleep_for(7);

   LOG_THREAD("[W3  ] locking mutex_C (should block until W2 releases)");
   mutex_C.lock();
   LOG_THREAD("[W3  ] acquired mutex_C after other waiters");

   rtk::Scheduler::sleep_for(5);

   LOG_THREAD("[W3  ] unlocking mutex_C");
   mutex_C.unlock();

   LOG_THREAD("[W3  ] finish. Parking");
   while (true) {
      rtk::Scheduler::sleep_for(100);
   }
}

// ---------------------------------------------------------------------------
// Monitor thread - stack high-water marks
// ---------------------------------------------------------------------------

static void print_stack_high_water(const char* name, const std::array<std::byte, STACK_BYTES>& stack, std::size_t reserved_bytes)
{
   static constexpr std::byte PATTERN{0xAA};

   // Count how many bytes from the *bottom* are still painted.
   std::size_t painted = 0;
   for (; painted < STACK_BYTES; ++painted) {
      if (stack[painted] != PATTERN) break;
   }

   std::size_t total_used = STACK_BYTES - painted;
   std::size_t user_used     =  total_used > reserved_bytes ? ( total_used - reserved_bytes) : 0;
   std::size_t user_capacity = STACK_BYTES > reserved_bytes ? (STACK_BYTES - reserved_bytes) : 0;

   LOG_THREAD("[MON ] %-4s stack usage: used=%zu bytes (of %zu user bytes)", name, user_used, user_capacity);
}

static void thread_MON_entry()
{
   LOG_THREAD("[MON ] enter");

   const std::size_t reserved = rtk::Thread::reserved_stack_size();

   while (true) {
      // You can change the interval as needed
      rtk::Scheduler::sleep_for(25);

      LOG_THREAD("[MON ] ----- Stack high-water marks -----");

      print_stack_high_water("LOW",  stack_LOW,  reserved);
      print_stack_high_water("MED",  stack_MED,  reserved);
      print_stack_high_water("HIGH", stack_HIGH, reserved);

      print_stack_high_water("OWNC", stack_OWNC, reserved);
      print_stack_high_water("W1",   stack_W1,   reserved);
      print_stack_high_water("W2",   stack_W2,   reserved);
      print_stack_high_water("W3",   stack_W3,   reserved);

      print_stack_high_water("MON",  stack_MON,  reserved);
   }
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------

int main()
{
   // Paint all stacks with a known pattern so the monitor can compute HWM.
   auto paint_stack = [](auto& stack) {
      stack.fill(std::byte{0xAA});
   };

   paint_stack(stack_LOW);
   paint_stack(stack_MED);
   paint_stack(stack_HIGH);
   paint_stack(stack_OWNC);
   paint_stack(stack_W1);
   paint_stack(stack_W2);
   paint_stack(stack_W3);
   paint_stack(stack_MON);

   rtk::Scheduler::init(10); // 10 ticks per second like before

   // PI chain threads
   rtk::Thread tL (rtk::Thread::Entry(thread_LOW_entry),       stack_LOW,  rtk::Thread::Priority(15));
   rtk::Thread tM (rtk::Thread::Entry(thread_MED_entry),       stack_MED,  rtk::Thread::Priority(8));
   rtk::Thread tH (rtk::Thread::Entry(thread_HIGH_entry),      stack_HIGH, rtk::Thread::Priority(1));

   // Multi-waiter / timeout threads on mutex_C
   // Owner is relatively low priority; waiters span a range above it.
   rtk::Thread tOwnC(rtk::Thread::Entry(thread_OWNER_C_entry), stack_OWNC, rtk::Thread::Priority(12));
   rtk::Thread tW1  (rtk::Thread::Entry(thread_W1_entry),      stack_W1,   rtk::Thread::Priority(2)); // highest waiter, times out
   rtk::Thread tW2  (rtk::Thread::Entry(thread_W2_entry),      stack_W2,   rtk::Thread::Priority(4)); // long waiter, eventually acquires
   rtk::Thread tW3  (rtk::Thread::Entry(thread_W3_entry),      stack_W3,   rtk::Thread::Priority(7)); // plain waiter

   // Monitor thread; modest priority so it runs when things are calmer
   rtk::Thread tMon (rtk::Thread::Entry(thread_MON_entry),     stack_MON,  rtk::Thread::Priority(13));

   rtk::Scheduler::start();
}
