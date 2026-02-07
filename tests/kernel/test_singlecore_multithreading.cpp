#include "cortos/kernel.hpp"
#include "cortos/config.hpp"
#include "cortos/port_traits.h"

#include "gtest/gtest.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace cortos;

static_assert(config::CORES == 1, "Test suite is designed for single core configuration only");

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


TEST(SingleCoreMultiThread_Test,
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

TEST(SingleCoreMultiThread_Test,
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


TEST(SingleCoreMultiThread_Test,
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

TEST(SingleCoreMultiThread_Test,
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

TEST(SingleCoreMultiThread_Test,
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
