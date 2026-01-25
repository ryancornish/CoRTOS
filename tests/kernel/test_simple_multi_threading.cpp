#include "cortos/kernel.hpp"

#include "gtest/gtest.h"

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


TEST(Given_a_single_core_and_two_threads_of_equal_priority, When_the_system_starts_Then_threads_execute_in_registration_order)
{
   // GIVEN:

   kernel::initialise();

   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack1;
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack2;

   std::vector<Thread::Id> entry_order;

   Thread t1(
      [&entry_order]{ entry_order.push_back(this_thread::id()); },
      stack1,
      Thread::Priority(0),
      Core0
   );

   Thread t2(
      [&entry_order]{ entry_order.push_back(this_thread::id()); },
      stack2,
      Thread::Priority(0),
      Core0
   );

   EXPECT_EQ(kernel::active_threads(), 2u) << "Diagnosis: Not all threads registered";

   // WHEN:

   kernel::start();

   // THEN:

   EXPECT_EQ(kernel::active_threads(), 0u)  << "Diagnosis: Not all threads terminated";
   EXPECT_EQ(entry_order.size(), 2u)        << "Diagnosis: Not all threads started";
   EXPECT_EQ(entry_order[0], Thread::Id(1)) << "Diagnosis: 't1' did not run first";
   EXPECT_EQ(entry_order[1], Thread::Id(2)) << "Diagnosis: 't2' did not run second";
}

// Can't uncomment until I figure out how to shutdown the kernel cleanly
// TEST(Given_a_single_core_and_two_threads_of_equal_priority, When_the_system_starts_and_each_thread_yeilds_to_each_other_over_time_Then_threads_cooperatively_progress)
// {
//    // GIVEN:

//    kernel::initialise();

//    alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack1;
//    alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack2;

//    std::vector<Thread::Id> entry_order;

//    Thread t1(
//       [&entry_order]{
//          for (int stage = 0; stage < 3; stage++) {
//             entry_order.push_back(this_thread::id());
//             this_thread::yield();
//          }
//       },
//       stack1,
//       Thread::Priority(0),
//       Core0
//    );

//    Thread t2(
//       [&entry_order]{
//          for (int stage = 0; stage < 3; stage++) {
//             entry_order.push_back(this_thread::id());
//             this_thread::yield();
//          }
//       },
//       stack2,
//       Thread::Priority(0),
//       Core0
//    );

//    EXPECT_EQ(kernel::active_threads(), 2u) << "Diagnosis: Not all threads registered";

//    // WHEN:

//    kernel::start();

//    // THEN:

//    EXPECT_EQ(kernel::active_threads(), 0u)  << "Diagnosis: Not all threads terminated";
//    EXPECT_EQ(entry_order.size(), 6u)        << "Diagnosis: Not all threads completed stages";
//    EXPECT_EQ(entry_order[0], Thread::Id(1)) << "Diagnosis: 't1' did not run first";
//    EXPECT_EQ(entry_order[1], Thread::Id(2)) << "Diagnosis: 't2' did not run second";
//    EXPECT_EQ(entry_order[2], Thread::Id(1)) << "Diagnosis: 't1' did not run third";
//    EXPECT_EQ(entry_order[3], Thread::Id(2)) << "Diagnosis: 't2' did not run fourth";
//    EXPECT_EQ(entry_order[4], Thread::Id(1)) << "Diagnosis: 't1' did not run fifth";
//    EXPECT_EQ(entry_order[5], Thread::Id(2)) << "Diagnosis: 't2' did not run sixth";
// }



