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
public:
   bool thread_entry_ran = false;

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

static void entry()
{
   std::printf("core %d: entry()\n", this_thread::core_id());
   active_fixture->thread_entry_ran = true;
}

TEST_F(Test, launch_single_thread)
{
   kernel::initialise();

   alignas(16) static std::array<std::byte, 16 * 1024> stack;

   Thread t1(
      entry,
      stack,
      Thread::Priority(0),
      Core0
   );

   kernel::start();

   ASSERT_TRUE(thread_entry_ran);
}

