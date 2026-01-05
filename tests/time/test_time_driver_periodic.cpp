/**
 * @file test_time_driver_periodic.cpp
 * @brief Unit tests for PeriodicTickDriver
 *
 * Only compiled when CORTOS_TIME_DRIVER=periodic or when building all tests
 */

#include "cortos/time_driver.hpp"
#include "cortos/time_driver_periodic.hpp"
#include "cortos/port.h"

#include <gtest/gtest.h>
#include <atomic>

using namespace cortos;

/* ============================================================================
 * Test Fixtures
 * ========================================================================= */

class PeriodicTimeDriverTest : public ::testing::Test
{
protected:
   void SetUp() override
   {
      cortos_port_time_reset(0);
   }

   void TearDown() override
   {
      ITimeDriver::set_instance(nullptr);
   }
};

/* ============================================================================
 * Basic Functionality Tests
 * ========================================================================= */

TEST_F(PeriodicTimeDriverTest, InitialTime)
{
   PeriodicTickDriver driver(1000);  // 1kHz
   ITimeDriver::set_instance(&driver);

   driver.start();

   EXPECT_EQ(driver.now().value, 0);
}

TEST_F(PeriodicTimeDriverTest, TimeAdvancesOnTick)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   EXPECT_EQ(driver.now().value, 0);

   // Simulate 5 ticks
   for (int i = 0; i < 5; i++)
   {
      driver.on_timer_isr();
   }

   EXPECT_EQ(driver.now().value, 5);
}

TEST_F(PeriodicTimeDriverTest, ScheduleAndFire)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   // Schedule callback at tick 10
   auto handle = driver.schedule_at(TimePoint{10}, callback, &callback_count);
   EXPECT_NE(handle.id, 0u);

   // Tick to 9 - shouldn't fire
   for (int i = 0; i < 9; i++)
   {
      driver.on_timer_isr();
   }
   EXPECT_EQ(callback_count.load(), 0);

   // Tick to 10 - should fire
   driver.on_timer_isr();
   EXPECT_EQ(callback_count.load(), 1);

   // Further ticks shouldn't fire again
   driver.on_timer_isr();
   EXPECT_EQ(callback_count.load(), 1);
}

TEST_F(PeriodicTimeDriverTest, MultipleCallbacks)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> count1{0}, count2{0}, count3{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   auto slot1 = driver.schedule_at(TimePoint{5}, callback, &count1);
   auto slot2 = driver.schedule_at(TimePoint{10}, callback, &count2);
   auto slot3 = driver.schedule_at(TimePoint{15}, callback, &count3);
   EXPECT_NE(slot1.id, 0);
   EXPECT_NE(slot2.id, 0);
   EXPECT_NE(slot3.id, 0);

   // Tick to 7
   for (int i = 0; i < 7; i++)
   {
      driver.on_timer_isr();
   }
   EXPECT_EQ(count1.load(), 1);
   EXPECT_EQ(count2.load(), 0);
   EXPECT_EQ(count3.load(), 0);

   // Tick to 12
   for (int i = 7; i < 12; i++)
   {
      driver.on_timer_isr();
   }
   EXPECT_EQ(count1.load(), 1);
   EXPECT_EQ(count2.load(), 1);
   EXPECT_EQ(count3.load(), 0);

   // Tick to 20
   for (int i = 12; i < 20; i++)
   {
      driver.on_timer_isr();
   }
   EXPECT_EQ(count1.load(), 1);
   EXPECT_EQ(count2.load(), 1);
   EXPECT_EQ(count3.load(), 1);
}

TEST_F(PeriodicTimeDriverTest, CallbackFiresAtExactTime)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   auto slot = driver.schedule_at(TimePoint{100}, callback, &callback_count);
   EXPECT_NE(slot.id, 0);

   // Tick exactly to 100
   for (int i = 0; i < 100; i++)
   {
      driver.on_timer_isr();
   }
   EXPECT_EQ(callback_count.load(), 1);
}

TEST_F(PeriodicTimeDriverTest, CallbackFiresIfScheduledInPast)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   // Advance time to 100
   for (int i = 0; i < 100; i++)
   {
      driver.on_timer_isr();
   }

   std::atomic<int> callback_count{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   // Schedule in the past
   auto slot = driver.schedule_at(TimePoint{50}, callback, &callback_count);
   EXPECT_NE(slot.id, 0);

   // Should fire on next tick
   driver.on_timer_isr();
   EXPECT_EQ(callback_count.load(), 1);
}

/* ============================================================================
 * Cancellation Tests
 * ========================================================================= */

TEST_F(PeriodicTimeDriverTest, CancelBeforeFiring)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   auto handle = driver.schedule_at(TimePoint{100}, callback, &callback_count);

   // Tick partway
   for (int i = 0; i < 50; i++)
   {
      driver.on_timer_isr();
   }

   // Cancel
   bool cancelled = driver.cancel(handle);
   EXPECT_TRUE(cancelled);

   // Tick past scheduled time
   for (int i = 50; i < 150; i++)
   {
      driver.on_timer_isr();
   }

   EXPECT_EQ(callback_count.load(), 0);  // Should not have fired
}

TEST_F(PeriodicTimeDriverTest, CancelAfterFiring)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   auto handle = driver.schedule_at(TimePoint{10}, callback, &callback_count);

   // Fire the callback
   for (int i = 0; i < 15; i++)
   {
      driver.on_timer_isr();
   }
   EXPECT_EQ(callback_count.load(), 1);

   // Try to cancel
   bool cancelled = driver.cancel(handle);
   EXPECT_FALSE(cancelled);  // Already fired
}

TEST_F(PeriodicTimeDriverTest, CancelInvalidHandle)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   ITimeDriver::Handle invalid{0};
   bool cancelled = driver.cancel(invalid);
   EXPECT_FALSE(cancelled);

   ITimeDriver::Handle nonexistent{99999};
   cancelled = driver.cancel(nonexistent);
   EXPECT_FALSE(cancelled);
}

TEST_F(PeriodicTimeDriverTest, CancelTwice)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1);
   };

   auto handle = driver.schedule_at(TimePoint{100}, callback, &callback_count);

   bool cancelled1 = driver.cancel(handle);
   EXPECT_TRUE(cancelled1);

   bool cancelled2 = driver.cancel(handle);
   EXPECT_FALSE(cancelled2);  // Already cancelled
}

/* ============================================================================
 * Duration Conversion Tests
 * ========================================================================= */

TEST_F(PeriodicTimeDriverTest, DurationConversion_1kHz)
{
   PeriodicTickDriver driver(1000);  // 1kHz

   Duration d1 = driver.from_milliseconds(10);
   EXPECT_EQ(d1.value, 10);  // 10ms = 10 ticks at 1kHz

   Duration d2 = driver.from_microseconds(5000);
   EXPECT_EQ(d2.value, 5);  // 5000us = 5ms = 5 ticks

   // Test rounding up
   Duration d3 = driver.from_microseconds(1001);
   EXPECT_EQ(d3.value, 2);  // Rounds up to 2 ticks
}

TEST_F(PeriodicTimeDriverTest, DurationConversion_100Hz)
{
   PeriodicTickDriver driver(100);  // 100Hz (10ms ticks)

   Duration d1 = driver.from_milliseconds(10);
   EXPECT_EQ(d1.value, 1);  // 10ms = 1 tick at 100Hz

   Duration d2 = driver.from_milliseconds(25);
   EXPECT_EQ(d2.value, 3);  // 25ms rounds up to 3 ticks (30ms)
}

TEST_F(PeriodicTimeDriverTest, TickFrequency)
{
   PeriodicTickDriver driver(2000);  // 2kHz

   // At 2kHz, 1ms = 2 ticks
   Duration d = driver.from_milliseconds(1);
   EXPECT_EQ(d.value, 2);
}

/* ============================================================================
 * Slot Exhaustion Tests
 * ========================================================================= */

TEST_F(PeriodicTimeDriverTest, MaxScheduledCallbacks)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1);
   };

   // Schedule MAX_SCHEDULED_CALLBACKS
   std::vector<ITimeDriver::Handle> handles;
   for (uint32_t i = 0; i < PeriodicTickDriver::MAX_SCHEDULED_CALLBACKS; i++)
   {
      auto handle = driver.schedule_at(TimePoint{i + 10}, callback, &callback_count);
      EXPECT_NE(handle.id, 0u);
      handles.push_back(handle);
   }

   // Next one should fail (return invalid handle)
   auto overflow_handle = driver.schedule_at(TimePoint{100}, callback, &callback_count);
   EXPECT_EQ(overflow_handle.id, 0u);

   // Cancel one
   driver.cancel(handles[0]);

   // Now we can schedule again
   auto new_handle = driver.schedule_at(TimePoint{100}, callback, &callback_count);
   EXPECT_NE(new_handle.id, 0u);
}

/* ============================================================================
 * Start/Stop Tests
 * ========================================================================= */

TEST_F(PeriodicTimeDriverTest, StartStop)
{
   PeriodicTickDriver driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   for (int i = 0; i < 10; i++)
   {
      driver.on_timer_isr();
   }
   EXPECT_EQ(driver.now().value, 10);

   driver.stop();

   // Can restart
   driver.start();

   for (int i = 0; i < 5; i++)
   {
      driver.on_timer_isr();
   }
   EXPECT_EQ(driver.now().value, 15);
}

TEST_F(PeriodicTimeDriverTest, SingletonAccess)
{
   PeriodicTickDriver driver(1000);

   ITimeDriver::set_instance(&driver);

   ITimeDriver& instance = ITimeDriver::get_instance();

   EXPECT_EQ(&instance, &driver);

   // Can use through singleton
   EXPECT_EQ(instance.now().value, 0);
}
