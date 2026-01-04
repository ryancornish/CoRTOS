/**
 * @file test_time_driver.cpp
 * @brief Unit tests for TimePoint and Duration types
 *
 * These tests don't depend on any specific TimeDriver implementation.
 */

#include "cortos/time_driver.hpp"
#include <gtest/gtest.h>

using namespace cortos;

/* ============================================================================
 * TimePoint and Duration Tests
 * ========================================================================= */

TEST(TimeTypesTest, TimePointComparison)
{
   TimePoint t1{100};
   TimePoint t2{200};
   TimePoint t3{100};

   EXPECT_TRUE(t1 < t2);
   EXPECT_TRUE(t1 <= t2);
   EXPECT_TRUE(t2 > t1);
   EXPECT_TRUE(t2 >= t1);
   EXPECT_TRUE(t1 == t3);
   EXPECT_TRUE(t1 != t2);
}

TEST(TimeTypesTest, DurationArithmetic)
{
   TimePoint t1{100};
   Duration d1{50};
   Duration d2{30};

   TimePoint t2 = t1 + d1;
   EXPECT_EQ(t2.value, 150);

   TimePoint t3 = d1 + t1;
   EXPECT_EQ(t3.value, 150);

   Duration d3 = d1 + d2;
   EXPECT_EQ(d3.value, 80);
}

TEST(TimeTypesTest, DurationBetween)
{
   TimePoint t1{100};
   TimePoint t2{150};

   Duration d1 = duration_between(t2, t1);
   EXPECT_EQ(d1.value, 50);

   // Reverse should give 0 (no negative durations)
   Duration d2 = duration_between(t1, t2);
   EXPECT_EQ(d2.value, 0);

   // Same time
   Duration d3 = duration_between(t1, t1);
   EXPECT_EQ(d3.value, 0);
}

TEST(TimeTypesTest, TimePointMax)
{
   TimePoint max = TimePoint::max();
   TimePoint any{12345};

   EXPECT_TRUE(max > any);
   EXPECT_TRUE(any < max);
   EXPECT_EQ(max.value, UINT64_MAX);
}

TEST(TimeTypesTest, DurationComparison)
{
   Duration d1{100};
   Duration d2{200};

   EXPECT_TRUE(d1 < d2);
   EXPECT_TRUE(d2 > d1);
   EXPECT_TRUE(d1 == Duration{100});
}