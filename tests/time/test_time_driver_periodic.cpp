/**
 * @file test_time_driver_periodic.cpp
 * @brief Unit tests for PeriodicTickDriver
 *
 * Only compiled when CORTOS_TIME_DRIVER=periodic
 */

#include "cortos/time_driver.hpp"
#include "cortos/time_driver_periodic.hpp"
#include <gtest/gtest.h>

using namespace cortos;

/* ============================================================================
 * Test Fixtures
 * ========================================================================= */

class PeriodicTimeDriverTest : public ::testing::Test
{
protected:
   void SetUp() override {}
};

/* ============================================================================
 * Periodic Tick Driver Tests
 * ========================================================================= */

TEST_F(PeriodicTimeDriverTest, TimeAdvancesOnTick)
{
   int callback_count = 0;

   PeriodicTickDriver driver(
      1'000,
      [](void* cb_count)
      {
         auto* callback_count = static_cast<int*>(cb_count);
         (*callback_count)++;
      }, &callback_count
   );

   driver.start();

   EXPECT_EQ(driver.now().value, 0);

   // Simulate 5 ticks
   for (int i = 0; i < 5; i++) {
      driver.on_tick_interrupt();
   }

   EXPECT_EQ(driver.now().value, 5);
   EXPECT_EQ(callback_count, 5);  // Callback fired 5 times
}

TEST_F(PeriodicTimeDriverTest, DurationConversion)
{
   PeriodicTickDriver driver(1'000, nullptr);  // 1kHz

   Duration d1 = driver.from_milliseconds(10);
   EXPECT_EQ(d1.value, 10);

   Duration d2 = driver.from_microseconds(5'000);
   EXPECT_EQ(d2.value, 5);
}

TEST_F(PeriodicTimeDriverTest, ScheduleWakeupIsNoop)
{
   PeriodicTickDriver driver(1'000, nullptr);

   driver.start();

   // schedule_wakeup should be a no-op for periodic driver
   driver.schedule_wakeup(TimePoint{1'000});

   // Should not affect time
   EXPECT_EQ(driver.now().value, 0);
}

TEST_F(PeriodicTimeDriverTest, TickFrequency)
{
   PeriodicTickDriver driver(2'000, nullptr);  // 2kHz

   EXPECT_EQ(driver.get_tick_frequency_hz(), 2'000);

   // At 2kHz, 1ms = 2 ticks
   Duration d = driver.from_milliseconds(1);
   EXPECT_EQ(d.value, 2);
}

TEST_F(PeriodicTimeDriverTest, SingletonAccess)
{
   PeriodicTickDriver driver(1'000, nullptr);

   ITimeDriver::set_instance(&driver);

   ITimeDriver& instance = ITimeDriver::get_instance();

   EXPECT_EQ(&instance, &driver);

   // Can use through singleton
   EXPECT_EQ(instance.now().value, 0);

   // Clean up
   ITimeDriver::set_instance(nullptr);
}
