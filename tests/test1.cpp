#include <gtest/gtest.h>
#include "cortos.hpp"

using cortos::JobModel;
using cortos::Config;

TEST(JobModel, InlineLambdaNoHeap)
{
   using SmallJob = JobModel<16, Config::JobHeapPolicy::NoHeap>;

   int hit = 0;
   {
      SmallJob job([&]{
         hit = 42;
      });

      EXPECT_TRUE(static_cast<bool>(job));
      job();
      EXPECT_EQ(hit, 42);
   }

   SmallJob job2([&]{ hit = 1; });
   job2.reset();
   job2();
   EXPECT_EQ(hit, 42);
}