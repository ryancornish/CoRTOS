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
     GivenTwoEqualPriorityThreads_WhenSystemStarts_ThenThreadsExecuteInRegistrationOrder)
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
     GivenTwoEqualPriorityThreads_WhenEachYields_ThenTheyCooperativelyProgressAlternating)
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
     GivenTwoDifferentPriorities_WhenSystemStarts_ThenHigherPriorityRunsFirstEvenIfRegisteredSecond)
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
     GivenThreeEqualPriorityThreads_WhenTheyYield_ThenTheyRoundRobinInRegistrationOrder)
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
     GivenTwoThreads_WhenOneNeverYields_ThenOtherDoesNotRunUntilFirstReturns)
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

TEST(SingleCoreMultiThread_Test,
     GivenThirtyEqualPriorityThreads_WhenSystemStarts_ThenThreadsObeyRoundRobinRules)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::array<std::byte, 16 * 1024>, 30> stacks{};


   std::vector<Thread> threads;
   threads.reserve(stacks.size());
   std::vector<uint32_t> markers;

   kernel::initialise();

   for (auto& stack : stacks) {
      threads.emplace_back(
      [&]{
         markers.push_back(this_thread::id());
         this_thread::yield();
         markers.push_back(this_thread::id());
      },
      stack, Thread::Priority(0), Core0);
   }

   kernel::start();

   ASSERT_EQ(markers.size(), 30u * 2);

   auto expected_order = std::to_array<uint32_t>({
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
   });

   for (unsigned i = 0; i < markers.size(); ++i) {
      EXPECT_EQ(markers[i], expected_order[i]);
   }

   kernel::finalise();
}


TEST(SingleCoreMultiThread_Test,
     GivenThirtyDifferentPriorityThreads_WhenSystemStarts_ThenThreadsExecuteInPriorityOrder)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::array<std::byte, 16 * 1024>, 30> stacks{};


   std::vector<Thread> threads;
   threads.reserve(stacks.size());
   std::vector<uint32_t> markers;

   kernel::initialise();

   for (unsigned prio = 29; auto& stack : stacks) {
      threads.emplace_back(
      [&]{
         markers.push_back(this_thread::id());
         this_thread::yield(); // Should reenqueue same task leading to double number pushback
         markers.push_back(this_thread::id());
      },
      stack, Thread::Priority(prio--), Core0);
   }

   kernel::start();

   ASSERT_EQ(markers.size(), 30u * 2);

   auto expected_order = std::to_array<uint32_t>({
      30, 30, 29, 29, 28, 28, 27, 27, 26, 26, 25, 25, 24, 24, 23, 23, 22, 22, 21, 21, 20, 20, 19, 19, 18, 18, 17, 17, 16, 16,
      15, 15, 14, 14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9, 8, 8, 7, 7, 6, 6, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1,
   });

   for (unsigned i = 0; i < markers.size(); ++i) {
      EXPECT_EQ(markers[i], expected_order[i]);
   }

   kernel::finalise();
}


TEST(SingleCoreMultiThread_Test,
    GivenSingleThread_WhenThreadCreatesAnotherThreadOfHigherPriority_ThenThreadIsImmediatelyPreempted)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s_creator{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s_child{};

   Thread child_thread;

   std::vector<int> marker;

   // GIVEN:

   kernel::initialise();

   Thread creator(
      [&]{
         marker.push_back(10); // 10 is the priority of the creator thread and marks when it ran

         child_thread = Thread(
            [&]{
               marker.push_back(9);
            },
            s_child,
            Thread::Priority(9),
            Core0
         );

         marker.push_back(10);
      },
      s_creator,
      Thread::Priority(10),
      Core0
   );

   // Only one thread should be registered (creator) as child_thread handle is empty
   EXPECT_EQ(kernel::active_threads(), 1u);

   // WHEN:

   kernel::start();

   // THEN:

   ASSERT_EQ(marker.size(), 3u);
   EXPECT_EQ(marker[0], 10u);
   EXPECT_EQ(marker[1], 9u)
      << "creator_thread was not preempted by just-created child_thread";
   EXPECT_EQ(marker[2], 10u);

   kernel::finalise();
}

TEST(SingleCoreMultiThread_Test,
    GivenSingleThread_WhenThreadCreatesAnotherThreadOfLowerPriority_ThenCreatedThreadDoesNotRunUntilFirstThreadFinishes)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s_creator{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s_child{};

   Thread child_thread;

   std::vector<int> marker;

   // GIVEN:

   kernel::initialise();

   Thread creator(
      [&]{
         marker.push_back(10); // 10 is the priority of the creator thread and marks when it ran

         child_thread = Thread(
            [&]{
               marker.push_back(11);
            },
            s_child,
            Thread::Priority(11),
            Core0
         );

         marker.push_back(10);
      },
      s_creator,
      Thread::Priority(10),
      Core0
   );

   // Only one thread should be registered (creator) as child_thread handle is empty
   EXPECT_EQ(kernel::active_threads(), 1u);

   // WHEN:

   kernel::start();

   // THEN:

   ASSERT_EQ(marker.size(), 3u);
   EXPECT_EQ(marker[0], 10u);
   EXPECT_EQ(marker[1], 10u)
      << "creator_thread was wrongfully preempted by just-created child_thread";
   EXPECT_EQ(marker[2], 11u);

   kernel::finalise();
}

TEST(SingleCoreMultiThread_Test,
    GivenSingleThread_WhenThreadCreatesAnotherThreadOfEqualPriority_ThenCreatedThreadDoesNotRunUntilFirstThreadYields)
{
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s_creator{};
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> s_child{};

   Thread child_thread;

   std::vector<uint32_t> marker;

   // GIVEN:

   kernel::initialise();

   Thread creator(
      [&]{
         marker.push_back(this_thread::id());

         child_thread = Thread(
            [&]{
               marker.push_back(this_thread::id());
               this_thread::yield();
               marker.push_back(this_thread::id());
            },
            s_child,
            Thread::Priority(10),
            Core0
         );

         marker.push_back(this_thread::id());
         this_thread::yield();
         marker.push_back(this_thread::id());
      },
      s_creator,
      Thread::Priority(10),
      Core0
   );

   // Only one thread should be registered (creator) as child_thread handle is empty
   EXPECT_EQ(kernel::active_threads(), 1u);

   // WHEN:

   kernel::start();

   // THEN:

   ASSERT_EQ(marker.size(), 5u);
   EXPECT_EQ(marker[0], 1u);
   EXPECT_EQ(marker[1], 1u);
   EXPECT_EQ(marker[2], 2u);
   EXPECT_EQ(marker[3], 1u);
   EXPECT_EQ(marker[4], 2u);

   kernel::finalise();
}

