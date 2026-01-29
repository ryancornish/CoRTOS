#include "cortos/kernel.hpp"

#include "gtest/gtest.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace cortos;

int main(int argc, char** argv)
{
   ::testing::InitGoogleTest(&argc, argv);

   int result = RUN_ALL_TESTS();

   return result;
}

struct ThreadSafeLog
{
   void push(Thread::Id id)
   {
      std::lock_guard<std::mutex> g(m);
      v.push_back(id);
   }
   std::vector<Thread::Id> v;
   std::mutex m;
};


TEST(SimpleMultiThreadingTest,
     GivenSingleCoreAndTwoEqualPriorityThreads_WhenSystemStarts_ThenThreadsExecuteInRegistrationOrder)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack1{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack2{};

   std::vector<Thread::Id> order;

   kernel::initialise();

   Thread t1([&]{ order.push_back(this_thread::id()); }, stack1, Thread::Priority(0), Core0);
   Thread t2([&]{ order.push_back(this_thread::id()); }, stack2, Thread::Priority(0), Core0);

   ASSERT_EQ(kernel::active_threads(), 2u) << "Not all threads registered";

   kernel::start();

   EXPECT_EQ(kernel::active_threads(), 0u) << "Not all threads terminated";
   ASSERT_EQ(order.size(), 2u)             << "Not all threads started";
   EXPECT_EQ(order[0], Thread::Id(1));
   EXPECT_EQ(order[1], Thread::Id(2));

   kernel::finalise();
}

TEST(SimpleMultiThreadingTest,
     GivenSingleCoreAndTwoEqualPriorityThreads_WhenEachYields_ThenTheyCooperativelyProgressAlternating)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack1{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack2{};

   std::vector<Thread::Id> order;

   auto worker = [&](int stages)
   {
      for (int i = 0; i < stages; ++i) {
         order.push_back(this_thread::id());
         this_thread::yield();
      }
   };

   kernel::initialise();

   Thread t1([&]{ worker(3); }, stack1, Thread::Priority(0), Core0);
   Thread t2([&]{ worker(3); }, stack2, Thread::Priority(0), Core0);

   ASSERT_EQ(kernel::active_threads(), 2u);

   kernel::start();

   EXPECT_EQ(kernel::active_threads(), 0u);
   ASSERT_EQ(order.size(), 6u);

   const std::array<Thread::Id, 6> expected{
      Thread::Id(1), Thread::Id(2), Thread::Id(1), Thread::Id(2), Thread::Id(1), Thread::Id(2)
   };
   for (size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(order[i], expected[i]) << "Mismatch at index " << i;
   }

   kernel::finalise();
}


TEST(SimpleMultiThreadingTest,
     GivenSingleCoreAndTwoDifferentPriorities_WhenSystemStarts_ThenHigherPriorityRunsFirstEvenIfRegisteredSecond)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack_lo{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack_hi{};

   std::vector<Thread::Id> order;

   kernel::initialise();

   // Lower priority first (numerically larger == lower priority in your code base as described)
   Thread low([&]{ order.push_back(this_thread::id()); }, stack_lo, Thread::Priority(5), Core0);
   Thread high([&]{ order.push_back(this_thread::id()); }, stack_hi, Thread::Priority(0), Core0);

   ASSERT_EQ(kernel::active_threads(), 2u);

   kernel::start();

   ASSERT_EQ(order.size(), 2u);
   EXPECT_EQ(order[0], Thread::Id(2)) << "High priority thread should run first";
   EXPECT_EQ(order[1], Thread::Id(1)) << "Low priority thread should run second";

   kernel::finalise();
}

TEST(SimpleMultiThreadingTest,
     GivenSingleCoreAndThreeEqualPriorityThreads_WhenTheyYield_ThenTheyRoundRobinInRegistrationOrder)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s1{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s2{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s3{};

   std::vector<Thread::Id> order;

   auto worker = [&](int stages)
   {
      for (int i = 0; i < stages; ++i) {
         order.push_back(this_thread::id());
         this_thread::yield();
      }
   };

   kernel::initialise();

   Thread t1([&]{ worker(3); }, s1, Thread::Priority(0), Core0);
   Thread t2([&]{ worker(3); }, s2, Thread::Priority(0), Core0);
   Thread t3([&]{ worker(3); }, s3, Thread::Priority(0), Core0);

   ASSERT_EQ(kernel::active_threads(), 3u);

   kernel::start();

   ASSERT_EQ(order.size(), 9u);

   const std::array<Thread::Id, 9> expected{
      Thread::Id(1), Thread::Id(2), Thread::Id(3),
      Thread::Id(1), Thread::Id(2), Thread::Id(3),
      Thread::Id(1), Thread::Id(2), Thread::Id(3),
   };

   for (size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(order[i], expected[i]) << "Mismatch at index " << i;
   }

   kernel::finalise();
}

TEST(SimpleMultiThreadingTest,
     GivenSingleCoreAndTwoThreads_WhenOneNeverYields_ThenOtherDoesNotRunUntilFirstReturns)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s1{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s2{};

   std::vector<int> markers;

   kernel::initialise();

   Thread t1(
      [&]{
         markers.push_back(1); // t1 start
         // No yield here; cooperatively hog until it returns.
         markers.push_back(2); // t1 end
      },
      s1,
      Thread::Priority(0),
      Core0
   );

   Thread t2(
      [&]{
         markers.push_back(3); // t2 start
      },
      s2,
      Thread::Priority(0),
      Core0
   );

   kernel::start();

   ASSERT_EQ(markers.size(), 3u);
   EXPECT_EQ(markers[0], 1);
   EXPECT_EQ(markers[1], 2);
   EXPECT_EQ(markers[2], 3) << "Second thread should only run after the first returns (cooperative)";

   kernel::finalise();
}

TEST(SimpleMultiThreadingTest,
     GivenSingleCore_WhenThreadReadsCoreId_ThenItIsCore0)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s1{};

   std::atomic<uint32_t> seen{0xFFFFFFFFu};

   kernel::initialise();

   Thread t(
      [&]{
         seen.store(this_thread::core_id(), std::memory_order_release);
      },
      s1,
      Thread::Priority(0),
      Core0
   );

   kernel::start();

   EXPECT_EQ(seen.load(std::memory_order_acquire), 0u);

   kernel::finalise();
}

// TEST(SimpleMultiThreadingTest,
//      GivenSingleCore_WhenThreadReadsTlsPointer_ThenItPointsToATcbLikeValue)
// {
//    // This is intentionally loose: your launcher sets TLS to the TCB address.
//    // We can at least assert it's non-null and stable across yields.
//    alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s1{};

//    std::atomic<void*> tls0{nullptr};
//    std::atomic<void*> tls1{nullptr};

//    kernel::initialise();

//    Thread t(
//       [&]{
//          void* a = cortos_port_get_tls_pointer();
//          tls0.store(a, std::memory_order_release);
//          this_thread::yield();
//          void* b = cortos_port_get_tls_pointer();
//          tls1.store(b, std::memory_order_release);
//       },
//       s1,
//       Thread::Priority(0),
//       Core0
//    );

//    kernel::start();

//    void* a = tls0.load(std::memory_order_acquire);
//    void* b = tls1.load(std::memory_order_acquire);
//    ASSERT_NE(a, nullptr);
//    EXPECT_EQ(a, b) << "TLS pointer should remain stable for the lifetime of the thread";

//    kernel::finalise();
// }


TEST(SimpleMultiThreadingTest,
     GivenTwoCoresAndThreadsPinnedToEachCore_WhenSystemStarts_ThenEachThreadRunsOnItsPinnedCore)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s1{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s2{};

   std::atomic<uint32_t> core_seen_t1{0xFFFFFFFFu};
   std::atomic<uint32_t> core_seen_t2{0xFFFFFFFFu};

   kernel::initialise();

   Thread t1(
      [&]{ core_seen_t1.store(this_thread::core_id(), std::memory_order_release); },
      s1,
      Thread::Priority(0),
      Core0
   );

   Thread t2(
      [&]{ core_seen_t2.store(this_thread::core_id(), std::memory_order_release); },
      s2,
      Thread::Priority(0),
      Core1
   );

   kernel::start();

   EXPECT_EQ(core_seen_t1.load(std::memory_order_acquire), 0u);
   EXPECT_EQ(core_seen_t2.load(std::memory_order_acquire), 1u);

   kernel::finalise();
}

TEST(SimpleMultiThreadingTest,
     GivenTwoCores_WhenCore0PostsWorkToCore1_ThenCore1ThreadRunsWithoutHangingInIdle)
{
   // This test is intentionally simple: if your cross-core inbox+IPI works,
   // a thread pinned to Core1 should be able to run even if Core1 starts in idle.
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s0{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s1{};

   std::atomic<bool> core1_ran{false};

   kernel::initialise();

   // Create core1 thread first so it may be queued while cores aren't running.
   Thread t_core1(
      [&]{ core1_ran.store(true, std::memory_order_release); },
      s1,
      Thread::Priority(0),
      Core1
   );

   // Core0 thread yields once to let the system settle.
   Thread t_core0(
      [&]{ this_thread::yield(); },
      s0,
      Thread::Priority(0),
      Core0
   );

   kernel::start();

   EXPECT_TRUE(core1_ran.load(std::memory_order_acquire))
      << "Core1 thread did not run (possible missing IPI / inbox drain / idle wake)";

   kernel::finalise();
}
