/**
 * @file test_time_driver_simulation.cpp
 * @brief Unit tests for SimulationTimeDriver
 */

#include "cortos/time_driver.hpp"
#include "cortos/time_driver_simulation.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using namespace cortos;

/* ============================================================================
 * Test Fixtures
 * ========================================================================= */

class SimulationTimeDriverTest : public ::testing::Test
{
protected:
   void SetUp() override {}
   void TearDown() override
   {
      ITimeDriver::set_instance(nullptr);
   }
};

/* ============================================================================
 * Virtual Mode - Basic Time Control
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, VirtualMode_InitialTimeIsZero)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   EXPECT_EQ(driver.now().value, 0);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_TimeAdvances)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   EXPECT_EQ(driver.now().value, 0);

   driver.advance_by(Duration{100});
   EXPECT_EQ(driver.now().value, 100);

   driver.advance_by(Duration{50});
   EXPECT_EQ(driver.now().value, 150);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_AdvanceTo)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   driver.advance_to(TimePoint{500});
   EXPECT_EQ(driver.now().value, 500);

   // Advancing to earlier time should clamp (no-op)
   driver.advance_to(TimePoint{200});
   EXPECT_EQ(driver.now().value, 500);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_MixedAdvanceSequence)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   driver.advance_by(Duration{10});
   driver.advance_by(Duration{20});
   driver.advance_to(TimePoint{100});
   driver.advance_by(Duration{50});

   EXPECT_EQ(driver.now().value, 150);
}

/* ============================================================================
 * Virtual Mode - Single Callback Tests
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, VirtualMode_CallbackFiresAtExactTime)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   std::atomic<int> count{0};
   auto callback = [](void* arg) {
      (*static_cast<std::atomic<int>*>(arg))++;
   };

   auto handle = driver.schedule_at(TimePoint{100}, callback, &count);
   EXPECT_NE(handle.id, 0u);

   // Advance to exactly 100
   driver.advance_to(TimePoint{100});
   EXPECT_EQ(count.load(), 1);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CallbackFiresWhenCrossing)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   std::atomic<int> count{0};
   auto callback = [](void* arg) { (*static_cast<std::atomic<int>*>(arg))++; };

   auto handle = driver.schedule_at(TimePoint{50}, callback, &count);
   EXPECT_NE(handle.id, 0u);

   // Advance past it
   driver.advance_to(TimePoint{150});
   EXPECT_EQ(count.load(), 1);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CallbackDoesNotFireBeforeTime)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   std::atomic<int> count{0};
   auto callback = [](void* arg) { (*static_cast<std::atomic<int>*>(arg))++; };

   auto handle = driver.schedule_at(TimePoint{100}, callback, &count);
   EXPECT_NE(handle.id, 0u);

   driver.advance_to(TimePoint{99});
   EXPECT_EQ(count.load(), 0);

   driver.advance_to(TimePoint{100});
   EXPECT_EQ(count.load(), 1);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CallbackFiresOnlyOnce)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   // Schedule callback at time 100
   auto handle = driver.schedule_at(TimePoint{100}, callback, &callback_count);
   EXPECT_NE(handle.id, 0u);

   // Advance to before callback
   driver.advance_to(TimePoint{50});
   EXPECT_EQ(callback_count.load(), 0);

   // Advance past callback
   driver.advance_to(TimePoint{150});
   EXPECT_EQ(callback_count.load(), 1);

   // Further advances shouldn't fire again
   driver.advance_to(TimePoint{200});
   EXPECT_EQ(callback_count.load(), 1);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CallbackInPastFiresImmediately)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   driver.advance_to(TimePoint{100});

   std::atomic<int> count{0};
   auto callback = [](void* arg) { (*static_cast<std::atomic<int>*>(arg))++; };

   // Schedule in the past
   auto handle = driver.schedule_at(TimePoint{50}, callback, &count);
   EXPECT_NE(handle.id, 0u);

   // Should fire on next advance
   driver.advance_by(Duration{1});
   EXPECT_EQ(count.load(), 1);
}

/* ============================================================================
 * Virtual Mode - Multiple Callback Tests
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, VirtualMode_MultipleCallbacks)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> count1{0}, count2{0}, count3{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   auto slot1 = driver.schedule_at(TimePoint{100}, callback, &count1);
   auto slot2 = driver.schedule_at(TimePoint{200}, callback, &count2);
   auto slot3 = driver.schedule_at(TimePoint{300}, callback, &count3);
   EXPECT_NE(slot1.id, 0u);
   EXPECT_NE(slot2.id, 0u);
   EXPECT_NE(slot3.id, 0u);

   driver.advance_to(TimePoint{150});
   EXPECT_EQ(count1.load(), 1);
   EXPECT_EQ(count2.load(), 0);
   EXPECT_EQ(count3.load(), 0);

   driver.advance_to(TimePoint{250});
   EXPECT_EQ(count1.load(), 1);
   EXPECT_EQ(count2.load(), 1);
   EXPECT_EQ(count3.load(), 0);

   driver.advance_to(TimePoint{350});
   EXPECT_EQ(count1.load(), 1);
   EXPECT_EQ(count2.load(), 1);
   EXPECT_EQ(count3.load(), 1);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_MultipleCallbacksFireInOrder)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   std::vector<int> fired_order;

   auto callback1 = [](void* arg) { static_cast<std::vector<int>*>(arg)->push_back(1); };
   auto callback2 = [](void* arg) { static_cast<std::vector<int>*>(arg)->push_back(2); };
   auto callback3 = [](void* arg) { static_cast<std::vector<int>*>(arg)->push_back(3); };

   auto h1 = driver.schedule_at(TimePoint{100}, callback1, &fired_order);
   auto h2 = driver.schedule_at(TimePoint{200}, callback2, &fired_order);
   auto h3 = driver.schedule_at(TimePoint{300}, callback3, &fired_order);
   EXPECT_NE(h1.id, 0u);
   EXPECT_NE(h2.id, 0u);
   EXPECT_NE(h3.id, 0u);

   driver.advance_to(TimePoint{150});
   EXPECT_EQ(fired_order.size(), 1u);
   EXPECT_EQ(fired_order[0], 1);

   driver.advance_to(TimePoint{250});
   EXPECT_EQ(fired_order.size(), 2u);
   EXPECT_EQ(fired_order[1], 2);

   driver.advance_to(TimePoint{350});
   EXPECT_EQ(fired_order.size(), 3u);
   EXPECT_EQ(fired_order[2], 3);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CallbacksAtSameTimeAllFire)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   std::atomic<int> count{0};
   auto callback = [](void* arg) { (*static_cast<std::atomic<int>*>(arg))++; };

   // Schedule 5 callbacks at same time
   for (int i = 0; i < 5; i++)
   {
      auto handle = driver.schedule_at(TimePoint{100}, callback, &count);
      EXPECT_NE(handle.id, 0u);
   }

   driver.advance_to(TimePoint{100});
   EXPECT_EQ(count.load(), 5);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_ManyCallbacksStressTest)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   std::atomic<int> count{0};
   auto callback = [](void* arg) { (*static_cast<std::atomic<int>*>(arg))++; };

   // Schedule 100 callbacks at various times
   for (int i = 0; i < 100; i++)
   {
      auto handle = driver.schedule_at(TimePoint{static_cast<uint64_t>(i * 10)}, callback, &count);
      EXPECT_NE(handle.id, 0u);
   }

   driver.advance_to(TimePoint{1000});
   EXPECT_EQ(count.load(), 100);
}

/* ============================================================================
 * Virtual Mode - Cancellation Tests
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, VirtualMode_CancelCallback)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   auto handle = driver.schedule_at(TimePoint{100}, callback, &callback_count);
   EXPECT_NE(handle.id, 0u);

   // Cancel before it fires
   bool cancelled = driver.cancel(handle);
   EXPECT_TRUE(cancelled);

   // Advance past callback time
   driver.advance_to(TimePoint{200});

   EXPECT_EQ(callback_count.load(), 0);  // Should not have fired
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CancelAfterFired)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   auto handle = driver.schedule_at(TimePoint{100}, callback, &callback_count);
   EXPECT_NE(handle.id, 0u);

   // Fire the callback
   driver.advance_to(TimePoint{150});
   EXPECT_EQ(callback_count.load(), 1);

   // Try to cancel after it fired
   bool cancelled = driver.cancel(handle);
   EXPECT_FALSE(cancelled);  // Already fired
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CancelInvalidHandle)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   ITimeDriver::Handle invalid{0};
   bool cancelled = driver.cancel(invalid);
   EXPECT_FALSE(cancelled);

   ITimeDriver::Handle nonexistent{99999};
   cancelled = driver.cancel(nonexistent);
   EXPECT_FALSE(cancelled);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CancelTwiceReturnsFalse)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   std::atomic<int> count{0};
   auto callback = [](void* arg) { (*static_cast<std::atomic<int>*>(arg))++; };

   auto handle = driver.schedule_at(TimePoint{100}, callback, &count);
   EXPECT_NE(handle.id, 0u);

   EXPECT_TRUE(driver.cancel(handle));
   EXPECT_FALSE(driver.cancel(handle));  // Second cancel fails
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CancelOneOfMany)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   std::atomic<int> count{0};
   auto callback = [](void* arg) { (*static_cast<std::atomic<int>*>(arg))++; };

   auto h1 = driver.schedule_at(TimePoint{100}, callback, &count);
   auto h2 = driver.schedule_at(TimePoint{200}, callback, &count);
   auto h3 = driver.schedule_at(TimePoint{300}, callback, &count);
   EXPECT_NE(h1.id, 0u);
   EXPECT_NE(h2.id, 0u);
   EXPECT_NE(h3.id, 0u);

   driver.cancel(h2);  // Cancel middle one

   driver.advance_to(TimePoint{400});
   EXPECT_EQ(count.load(), 2);  // Only h1 and h3 fired
}

/* ============================================================================
 * Virtual Mode - Edge Cases
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, VirtualMode_NullCallbackReturnsInvalidHandle)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   auto handle = driver.schedule_at(TimePoint{100}, nullptr, nullptr);
   EXPECT_EQ(handle.id, 0u);
}

TEST_F(SimulationTimeDriverTest, VirtualMode_CallbackCanScheduleAnother)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   driver.start();

   std::atomic<int> count{0};

   struct Context {
      SimulationTimeDriver<TimeMode::Virtual>* driver;
      std::atomic<int>* count;
   };

   Context ctx{&driver, &count};

   auto callback = [](void* arg) {
      auto* ctx = static_cast<Context*>(arg);
      (*ctx->count)++;

      // Schedule another callback
      auto h = ctx->driver->schedule_at(TimePoint{200},
         [](void* arg) { (*static_cast<std::atomic<int>*>(arg))++; },
         ctx->count);
      (void)h;  // Suppress nodiscard warning
   };

   auto h = driver.schedule_at(TimePoint{100}, callback, &ctx);
   EXPECT_NE(h.id, 0u);

   driver.advance_to(TimePoint{100});
   EXPECT_EQ(count.load(), 1);

   driver.advance_to(TimePoint{200});
   EXPECT_EQ(count.load(), 2);
}

/* ============================================================================
 * Duration Conversion Tests
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, DurationConversion_1kHz)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);  // 1kHz

   Duration d1 = driver.from_milliseconds(10);
   EXPECT_EQ(d1.value, 10);  // 10ms = 10 ticks at 1kHz

   Duration d2 = driver.from_microseconds(5000);
   EXPECT_EQ(d2.value, 5);  // 5000us = 5ms = 5 ticks at 1kHz

   // Test rounding up
   Duration d3 = driver.from_microseconds(1001);
   EXPECT_EQ(d3.value, 2);  // Rounds up to 2 ticks
}

TEST_F(SimulationTimeDriverTest, DurationConversion_1MHz)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1'000'000);  // 1MHz

   Duration d1 = driver.from_milliseconds(1);
   EXPECT_EQ(d1.value, 1000);  // 1ms = 1000 ticks at 1MHz

   Duration d2 = driver.from_microseconds(500);
   EXPECT_EQ(d2.value, 500);  // 500us = 500 ticks at 1MHz
}

/* ============================================================================
 * Start/Stop Tests
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, StartStop)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();
   driver.advance_to(TimePoint{100});
   EXPECT_EQ(driver.now().value, 100);

   driver.stop();

   // Can restart
   driver.start();
   driver.advance_to(TimePoint{200});
   EXPECT_EQ(driver.now().value, 200);
}

/* ============================================================================
 * RealTime Mode Tests
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, RealTimeMode_TimeProgresses)
{
   SimulationTimeDriver<TimeMode::RealTime> driver(1000);  // 1kHz
   ITimeDriver::set_instance(&driver);

   driver.start();

   TimePoint t1 = driver.now();

   // Sleep for a bit
   std::this_thread::sleep_for(std::chrono::milliseconds(20));

   TimePoint t2 = driver.now();

   // Time should have advanced
   EXPECT_GT(t2.value, t1.value);

   driver.stop();
}

TEST_F(SimulationTimeDriverTest, RealTimeMode_CallbackFires)
{
   SimulationTimeDriver<TimeMode::RealTime> driver(1000);
   ITimeDriver::set_instance(&driver);

   driver.start();

   std::atomic<int> callback_count{0};

   auto callback = [](void* arg) {
      auto* count = static_cast<std::atomic<int>*>(arg);
      count->fetch_add(1);
   };

   TimePoint now = driver.now();
   TimePoint future = TimePoint{now.value + 10};  // 10 ticks = 10ms at 1kHz

   auto slot = driver.schedule_at(future, callback, &callback_count);
   EXPECT_NE(slot.id, 0u);

   // Wait for callback to fire
   std::this_thread::sleep_for(std::chrono::milliseconds(30));

   EXPECT_EQ(callback_count.load(), 1);

   driver.stop();
}

TEST_F(SimulationTimeDriverTest, RealTimeMode_CancelBeforeAutoFire)
{
   SimulationTimeDriver<TimeMode::RealTime> driver(1000);
   driver.start();

   std::atomic<bool> fired{false};
   auto callback = [](void* arg) { *static_cast<std::atomic<bool>*>(arg) = true; };

   TimePoint now = driver.now();
   auto handle = driver.schedule_at(TimePoint{now.value + 100}, callback, &fired);
   EXPECT_NE(handle.id, 0u);

   // Cancel immediately
   bool cancelled = driver.cancel(handle);
   EXPECT_TRUE(cancelled);

   // Wait to ensure it doesn't fire
   std::this_thread::sleep_for(std::chrono::milliseconds(150));

   EXPECT_FALSE(fired.load());

   driver.stop();
}

/* ============================================================================
 * Singleton Tests
 * ========================================================================= */

TEST_F(SimulationTimeDriverTest, SingletonAccess)
{
   SimulationTimeDriver<TimeMode::Virtual> driver(1000);

   ITimeDriver::set_instance(&driver);

   ITimeDriver& instance = ITimeDriver::get_instance();

   EXPECT_EQ(&instance, &driver);

   // Can use through singleton
   EXPECT_EQ(instance.now().value, 0);
}
