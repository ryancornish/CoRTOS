#include "cortos.hpp"
#include "port_traits.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <format>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

// --- Mutexes -----------------------------------------------------
static constinit cortos::Mutex mutex_A; // For PI chain: MED <-> HIGH
static constinit cortos::Mutex mutex_B; // For PI chain: LOW  <-> MED
static constinit cortos::Mutex mutex_C; // For multi-waiter + timeout test

// --- Snacks for threads -----------------------------------------------------
static constexpr std::size_t STACK_BYTES = 1024 * 4;

alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_LOW{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_MED{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_HIGH{};

alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_OWNC{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_W1{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_W2{};
alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_W3{};

alignas(CORTOS_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_MON{}; // monitor

// ---------------------------------------------------------------------------
// Thread entries - Priority Inheritance Chain (LOW, MED, HIGH)
// ---------------------------------------------------------------------------

// Low priority: holds B for a long time.
static void thread_LOW_entry()
{
   LOG_TEST("[LOW ] enter");

   // Let others get created and maybe arrange their sleeps
   cortos::Scheduler::sleep_for(1);

   LOG_TEST("[LOW ] locking mutex_B");
   mutex_B.lock();
   LOG_TEST("[LOW ] acquired mutex_B, holding for 50 ticks");

   cortos::Scheduler::sleep_for(50);

   LOG_TEST("[LOW ] unlocking mutex_B");
   mutex_B.unlock();

   LOG_TEST("[LOW ] finish. Parking");
}

// Medium priority: lock A, then later try to lock B (blocked by LOW)
static void thread_MED_entry()
{
   LOG_TEST("[MED ] enter");

   // Give LOW time to start and grab B first
   cortos::Scheduler::sleep_for(5);

   LOG_TEST("[MED ] locking mutex_A");
   mutex_A.lock();
   LOG_TEST("[MED ] acquired mutex_A, holding for 5 ticks");

   cortos::Scheduler::sleep_for(5);

   LOG_TEST("[MED ] trying to lock mutex_B (should block, LOW holds it)");
   mutex_B.lock();
   LOG_TEST("[MED ] acquired mutex_B after blocking");

   cortos::Scheduler::sleep_for(5);

   LOG_TEST("[MED ] unlocking mutex_B");
   mutex_B.unlock();

   LOG_TEST("[MED ] unlocking mutex_A");
   mutex_A.unlock();

   LOG_TEST("[MED ] finish. Parking");
}

// High priority: eventually tries to lock A (owned by MED)
static void thread_HIGH_entry()
{
   LOG_TEST("[HIGH] enter");

   // Let MED first lock A, and LOW already have B
   cortos::Scheduler::sleep_for(20);

   LOG_TEST("[HIGH] trying to lock mutex_A (will block on MED, which is blocked on LOW)");
   mutex_A.lock();
   LOG_TEST("[HIGH] acquired mutex_A after PI chain resolved");

   cortos::Scheduler::sleep_for(5);

   LOG_TEST("[HIGH] unlocking mutex_A");
   mutex_A.unlock();

   LOG_TEST("[HIGH] finish. Parking");
}

// ---------------------------------------------------------------------------
// Thread entries - Multi-waiter + timeout on mutex_C
// ---------------------------------------------------------------------------

// Owner of mutex_C: low-ish priority, holds it for a while
static void thread_OWNER_C_entry()
{
   LOG_TEST("[OWNC] enter");

   LOG_TEST("[OWNC] locking mutex_C");
   mutex_C.lock();
   LOG_TEST("[OWNC] acquired mutex_C, holding for 40 ticks");

   cortos::Scheduler::sleep_for(40);

   LOG_TEST("[OWNC] unlocking mutex_C");
   mutex_C.unlock();

   LOG_TEST("[OWNC] finish. Parking");
}

// High-priority waiter with a *short* timeout: donates briefly, then times out
static void thread_W1_entry()
{
   LOG_TEST("[W1  ] enter");

   // Wait a bit so OWNER_C has mutex_C
   cortos::Scheduler::sleep_for(5);

   LOG_TEST("[W1  ] try_lock_for(5) on mutex_C (should TIMEOUT while donating)");
   bool got = mutex_C.try_lock_for(5);
   if (got) {
      LOG_TEST("[W1  ] UNEXPECTEDLY acquired mutex_C");
      mutex_C.unlock();
   } else {
      LOG_TEST("[W1  ] timed out on mutex_C, no longer waiting");
   }

   LOG_TEST("[W1  ] finish. Parking");
}

// Medium-high waiter with a long timeout: should eventually acquire mutex_C
static void thread_W2_entry()
{
   LOG_TEST("[W2  ] enter");

   // Arrive shortly after W1
   cortos::Scheduler::sleep_for(6);

   LOG_TEST("[W2  ] try_lock_for(50) on mutex_C (should acquire after OWNER_C unlocks)");
   bool got = mutex_C.try_lock_for(50);
   if (got) {
      LOG_TEST("[W2  ] acquired mutex_C after waiting");
      cortos::Scheduler::sleep_for(5);
      LOG_TEST("[W2  ] unlocking mutex_C");
      mutex_C.unlock();
   } else {
      LOG_TEST("[W2  ] UNEXPECTED timeout on mutex_C");
   }

   LOG_TEST("[W2  ] finish. Parking");
}

// Lower-priority waiter: plain lock, should be last to acquire mutex_C
static void thread_W3_entry()
{
   LOG_TEST("[W3  ] enter");

   // Arrive after W2 - still before OWNER_C unlocks
   cortos::Scheduler::sleep_for(7);

   LOG_TEST("[W3  ] locking mutex_C (should block until W2 releases)");
   mutex_C.lock();
   LOG_TEST("[W3  ] acquired mutex_C after other waiters");

   cortos::Scheduler::sleep_for(5);

   LOG_TEST("[W3  ] unlocking mutex_C");
   mutex_C.unlock();

   LOG_TEST("[W3  ] finish. Parking");
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

   LOG_TEST("[MON ] %-4s stack usage: used=%zu bytes (of %zu user bytes)", name, user_used, user_capacity);
}

static void thread_MON_entry()
{
   LOG_TEST("[MON ] enter");

   const std::size_t reserved = cortos::Thread::reserved_stack_size();

   while (true) {
      // You can change the interval as needed
      cortos::Scheduler::sleep_for(25);

      LOG_TEST("[MON ] ----- Stack high-water marks -----");

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

   cortos::Scheduler::init(10); // 10 ticks per second like before

   // PI chain threads
   cortos::Thread tL (cortos::Thread::Entry(thread_LOW_entry),       stack_LOW,  cortos::Thread::Priority(15));
   cortos::Thread tM (cortos::Thread::Entry(thread_MED_entry),       stack_MED,  cortos::Thread::Priority(8));
   cortos::Thread tH (cortos::Thread::Entry(thread_HIGH_entry),      stack_HIGH, cortos::Thread::Priority(1));

   // Multi-waiter / timeout threads on mutex_C
   // Owner is relatively low priority; waiters span a range above it.
   cortos::Thread tOwnC(cortos::Thread::Entry(thread_OWNER_C_entry), stack_OWNC, cortos::Thread::Priority(12));
   cortos::Thread tW1  (cortos::Thread::Entry(thread_W1_entry),      stack_W1,   cortos::Thread::Priority(2)); // highest waiter, times out
   cortos::Thread tW2  (cortos::Thread::Entry(thread_W2_entry),      stack_W2,   cortos::Thread::Priority(4)); // long waiter, eventually acquires
   cortos::Thread tW3  (cortos::Thread::Entry(thread_W3_entry),      stack_W3,   cortos::Thread::Priority(7)); // plain waiter

   // Monitor thread; modest priority so it runs when things are calmer
   cortos::Thread tMon (cortos::Thread::Entry(thread_MON_entry),     stack_MON,  cortos::Thread::Priority(13));

   cortos::Scheduler::start();
}
