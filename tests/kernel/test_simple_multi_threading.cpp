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


static class Test* active_fixture = nullptr;
class Test : public ::testing::Test
{
protected:
   void SetUp() override
   {
      active_fixture = this;
   }
   void TearDown() override
   {
      active_fixture = nullptr;
   }
};

TEST_F(Test, two_threads_run_concurrently)
{
   kernel::initialise();

   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack1;
   alignas(CORTOS_PORT_STACK_ALIGN) static std::array<std::byte, 16 * 1024> stack2;

   bool t1_entry_ran = false;
   bool t2_entry_ran = false;

   Thread t1(
      [&t1_entry_ran]{ t1_entry_ran = true; },
      stack1,
      Thread::Priority(0),
      Core0
   );

   Thread t2(
      [&t2_entry_ran]{ t2_entry_ran = true; },
      stack2,
      Thread::Priority(0),
      Core0
   );

   kernel::start();

   EXPECT_TRUE(t1_entry_ran);
   EXPECT_TRUE(t2_entry_ran);
}








