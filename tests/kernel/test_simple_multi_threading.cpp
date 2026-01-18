#include "cortos/kernel.hpp"

#include "gtest/gtest.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>


class Fixture : public ::testing::Test
{
protected:
   void SetUp() override
   {

   }
   void TearDown() override
   {

   }
};

TEST_F(Fixture, two_threads_run_concurrently)
{

}








int main(int argc, char** argv)
{
   ::testing::InitGoogleTest(&argc, argv);

   int result = RUN_ALL_TESTS();

   return result;
}
