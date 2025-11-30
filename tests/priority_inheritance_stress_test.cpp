#include <cornishrtk.hpp>
#include <port_traits.h>

#include <array>
#include <cstddef>
#include <cstdio>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

static constinit rtk::Mutex mutex_A;
static constinit rtk::Mutex mutex_B;

static constexpr std::size_t STACK_BYTES = 4096;

// Stacks for threads
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_LOW{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_MED{};
alignas(RTK_STACK_ALIGN) static constinit std::array<std::byte, STACK_BYTES> stack_HIGH{};

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

// Medium priority: lock A, then later try to lock B (blocked by L)
static void thread_MED_entry()
{
   LOG_THREAD("[MED ] enter");

   // Give L time to start and grab B first
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

// High priority: eventually tries to lock A (owned by M)
static void thread_HIGH_entry()
{
   LOG_THREAD("[HIGH] enter");

   // Let M first lock A, and L already have B
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

int main()
{
   rtk::Scheduler::init(10);

   rtk::Thread tL(rtk::Thread::Entry(thread_LOW_entry), stack_LOW, rtk::Thread::Priority(15));
   rtk::Thread tM(rtk::Thread::Entry(thread_MED_entry), stack_MED, rtk::Thread::Priority(8));
   rtk::Thread tH(rtk::Thread::Entry(thread_HIGH_entry), stack_HIGH, rtk::Thread::Priority(1));

   rtk::Scheduler::start();
}
