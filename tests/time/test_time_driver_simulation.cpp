/**
 * @file test_time_driver_simulation.cpp
 * @brief Unit tests for SimulationTimeDriver
 *
 * Only compiled when CORTOS_TIME_DRIVER=simulation
 */

#include "cortos/time_driver.hpp"
#include "cortos/time_driver_simulation.hpp"
#include <gtest/gtest.h>

using namespace cortos;

/* ============================================================================
 * Test Fixtures
 * ========================================================================= */

class SimulationTimeDriverTest : public ::testing::Test
{
protected:
   void SetUp() override {}
};

/* ============================================================================
 * Simulation TimeDriver Tests (Virtual Mode)
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, TimeAdvances)
{
   int callback_count = 0;

   SimulationTimeDriver<TimeMode::Virtual> driver([&callback_count]() { callback_count++; }, 1'000);

   driver.start();

   EXPECT_EQ(driver.now().value, 0);

   driver.advance_time(100);
   EXPECT_EQ(driver.now().value, 100);

   driver.advance_time(50);
   EXPECT_EQ(driver.now().value, 150);
}

TEST_F(SimulationTimeDriverTest, WakeupCallback)
{
   int callback_count = 0;

   SimulationTimeDriver<TimeMode::Virtual> driver([&callback_count]() { callback_count++; }, 1'000);

   driver.start();

   // Schedule wakeup at time 100
   driver.schedule_wakeup(TimePoint{100});

   // Advance to before wakeup
   driver.advance_time(50);
   EXPECT_EQ(callback_count, 0);  // Not fired yet

   // Advance past wakeup
   driver.advance_time(60);  // Now at time 110
   EXPECT_EQ(callback_count, 1);  // Fired once

   // Advancing more shouldn't fire again
   driver.advance_time(100);
   EXPECT_EQ(callback_count, 1);  // Still just once
}

TEST_F(SimulationTimeDriverTest, AdvanceTo)
{
   int callback_count = 0;

   SimulationTimeDriver<TimeMode::Virtual> driver([&callback_count]() { callback_count++; }, 1'000);

   SimulationTimeDriver<TimeMode::RealTime> driver2([&callback_count]() { callback_count++; }, 1'000);

   driver.start();
   driver.schedule_wakeup(TimePoint{500});

   // Jump directly to time 600
   driver.advance_to(TimePoint{600});

   EXPECT_EQ(driver.now().value, 600);
   EXPECT_EQ(callback_count, 1);  // Wakeup fired
}

TEST_F(SimulationTimeDriverTest, CancelWakeup)
{
   int callback_count = 0;

   SimulationTimeDriver<TimeMode::Virtual> driver([&callback_count]() { callback_count++; }, 1'000);

   driver.start();
   driver.schedule_wakeup(TimePoint{100});

   // Cancel before it fires
   driver.cancel_wakeup();

   // Advance past the original wakeup time
   driver.advance_time(200);

   EXPECT_EQ(callback_count, 0);  // Should not have fired
}

TEST_F(SimulationTimeDriverTest, DurationConversion)
{
   // 1kHz = 1 tick per ms
   SimulationTimeDriver<TimeMode::Virtual> driver(nullptr, 1'000);

   Duration d1 = driver.from_milliseconds(10);
   EXPECT_EQ(d1.value, 10);  // 10ms = 10 ticks at 1kHz

   Duration d2 = driver.from_microseconds(5'000);
   EXPECT_EQ(d2.value, 5);  // 5000us = 5ms = 5 ticks at 1kHz
}

TEST_F(SimulationTimeDriverTest, HighFrequency)
{
   // 1MHz = 1 tick per us
   SimulationTimeDriver<TimeMode::Virtual> driver(nullptr, 1'000'000);

   Duration d1 = driver.from_milliseconds(1);
   EXPECT_EQ(d1.value, 1000);  // 1ms = 1000 ticks at 1MHz

   Duration d2 = driver.from_microseconds(500);
   EXPECT_EQ(d2.value, 500);  // 500us = 500 ticks at 1MHz
}

TEST_F(SimulationTimeDriverTest, SingletonAccess)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(nullptr, 1000);

   ITimeDriver::set_instance(&driver);

   ITimeDriver& instance = ITimeDriver::get_instance();

   EXPECT_EQ(&instance, &driver);

   // Can use through singleton
   EXPECT_EQ(instance.now().value, 0);

   // Clean up
   ITimeDriver::set_instance(nullptr);
}
