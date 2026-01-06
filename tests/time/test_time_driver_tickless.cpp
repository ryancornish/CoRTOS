/**
 * @file test_time_driver_tickless.cpp
 * @brief Unit tests for TicklessDriver (pure model)
 *
 * Assumptions (Linux unit-test port backend):
 * - cortos_port_time_now() returns a monotonic counter in "port ticks"
 * - cortos_port_time_freq_hz() defines tick rate (recommended 1'000'000 => 1 tick = 1 us)
 * - cortos_port_time_reset(t) resets the counter deterministically
 * - (Linux-only) cortos_port_time_advance(delta) advances the counter deterministically
 *
 * These tests intentionally DO NOT require the port to actually deliver IRQs.
 * We "pump" by directly calling driver.on_timer_isr() after advancing time.
 *
 * Only compiled when CORTOS_TIME_DRIVER=tickless or when building all tests.
 */

#include "cortos/time_driver.hpp"
#include "cortos/time_driver_tickless.hpp"
#include "cortos/port.h"

#include <gtest/gtest.h>
#include <atomic>
#include <vector>

using namespace cortos;

// Linux-only test hook (implemented in the Linux port backend)
extern "C" void cortos_port_time_advance(uint64_t delta);

class TicklessTimeDriverTest : public ::testing::Test
{
protected:
   void SetUp() override
   {
      // Deterministic start
      cortos_port_time_reset(0);
   }

   void TearDown() override
   {
      ITimeDriver::set_instance(nullptr);
   }

   // Helper: advance port time and pump the driver once (like delivering an IRQ)
   static void advance_and_pump(TicklessDriver& driver, uint64_t delta_ticks)
   {
      cortos_port_time_advance(delta_ticks);
      driver.on_timer_isr();
   }
};

/* ============================================================================
 * Basic Functionality Tests
 * ========================================================================= */

TEST_F(TicklessTimeDriverTest, InitialTime)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);

   driver.start();
   EXPECT_EQ(driver.now().value, 0u);
}

TEST_F(TicklessTimeDriverTest, NowReflectsPortTime)
{
   TicklessDriver driver;
   driver.start();

   EXPECT_EQ(driver.now().value, 0u);

   cortos_port_time_advance(123);
   EXPECT_EQ(driver.now().value, 123u);

   cortos_port_time_advance(7);
   EXPECT_EQ(driver.now().value, 130u);
}

/* ============================================================================
 * Scheduling / Firing Tests
 * ========================================================================= */

TEST_F(TicklessTimeDriverTest, ScheduleAndFire_Absolute)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
   };

   // Schedule at absolute time 100
   auto h = driver.schedule_at(TimePoint{100}, callback, &callback_count);
   EXPECT_NE(h.id, 0u);

   // Advance to 99: shouldn't fire
   advance_and_pump(driver, 99);
   EXPECT_EQ(callback_count.load(), 0);

   // Advance to 100: should fire
   advance_and_pump(driver, 1);
   EXPECT_EQ(callback_count.load(), 1);

   // Further time: should not fire again
   advance_and_pump(driver, 100);
   EXPECT_EQ(callback_count.load(), 1);
}

TEST_F(TicklessTimeDriverTest, ScheduleAndFire_RelativeUsingConversions)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
   };

   // Schedule in +10ms
   const auto deadline = driver.now().value + driver.from_milliseconds(10).value;
   auto h = driver.schedule_at(TimePoint{deadline}, callback, &callback_count);
   EXPECT_NE(h.id, 0u);

   // Advance just before deadline
   advance_and_pump(driver, driver.from_milliseconds(10).value - 1);
   EXPECT_EQ(callback_count.load(), 0);

   // Hit deadline
   advance_and_pump(driver, 1);
   EXPECT_EQ(callback_count.load(), 1);
}

TEST_F(TicklessTimeDriverTest, MultipleCallbacks_FireInOrder)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   std::atomic<int> c1{0}, c2{0}, c3{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
   };

   auto h1 = driver.schedule_at(TimePoint{50},  callback, &c1);
   auto h2 = driver.schedule_at(TimePoint{100}, callback, &c2);
   auto h3 = driver.schedule_at(TimePoint{150}, callback, &c3);
   EXPECT_NE(h1.id, 0u);
   EXPECT_NE(h2.id, 0u);
   EXPECT_NE(h3.id, 0u);

   // To 60: only c1 fires
   advance_and_pump(driver, 60);
   EXPECT_EQ(c1.load(), 1);
   EXPECT_EQ(c2.load(), 0);
   EXPECT_EQ(c3.load(), 0);

   // To 120: c2 fires
   advance_and_pump(driver, 60); // total 120
   EXPECT_EQ(c1.load(), 1);
   EXPECT_EQ(c2.load(), 1);
   EXPECT_EQ(c3.load(), 0);

   // To 200: c3 fires
   advance_and_pump(driver, 80); // total 200
   EXPECT_EQ(c1.load(), 1);
   EXPECT_EQ(c2.load(), 1);
   EXPECT_EQ(c3.load(), 1);
}

TEST_F(TicklessTimeDriverTest, CallbackFiresAtExactTime)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
   };

   auto h = driver.schedule_at(TimePoint{10}, callback, &callback_count);
   EXPECT_NE(h.id, 0u);

   // Advance exactly to 10
   advance_and_pump(driver, 10);
   EXPECT_EQ(callback_count.load(), 1);
}

TEST_F(TicklessTimeDriverTest, CallbackFiresIfScheduledInPast_OnNextPump)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   // Advance time to 100
   cortos_port_time_advance(100);
   EXPECT_EQ(driver.now().value, 100u);

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
   };

   // Schedule at time 50 (in the past relative to now=100)
   auto h = driver.schedule_at(TimePoint{50}, callback, &callback_count);
   EXPECT_NE(h.id, 0u);

   // Should fire on the next ISR pump (even with delta=0)
   driver.on_timer_isr();
   EXPECT_EQ(callback_count.load(), 1);
}

/* ============================================================================
 * Cancellation Tests
 * ========================================================================= */

TEST_F(TicklessTimeDriverTest, CancelBeforeFiring)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
   };

   auto h = driver.schedule_at(TimePoint{100}, callback, &callback_count);
   EXPECT_NE(h.id, 0u);

   // Cancel before time reaches 100
   bool cancelled = driver.cancel(h);
   EXPECT_TRUE(cancelled);

   // Advance past deadline and pump: should not fire
   advance_and_pump(driver, 200);
   EXPECT_EQ(callback_count.load(), 0);
}

TEST_F(TicklessTimeDriverTest, CancelAfterFiring)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
   };

   auto h = driver.schedule_at(TimePoint{10}, callback, &callback_count);
   EXPECT_NE(h.id, 0u);

   // Fire
   advance_and_pump(driver, 10);
   EXPECT_EQ(callback_count.load(), 1);

   // Cancel after firing should return false (slot already freed)
   bool cancelled = driver.cancel(h);
   EXPECT_FALSE(cancelled);
}

TEST_F(TicklessTimeDriverTest, CancelInvalidHandle)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   ITimeDriver::Handle invalid{0};
   EXPECT_FALSE(driver.cancel(invalid));

   ITimeDriver::Handle nonexistent{99999};
   EXPECT_FALSE(driver.cancel(nonexistent));
}

TEST_F(TicklessTimeDriverTest, CancelTwice)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
   };

   auto h = driver.schedule_at(TimePoint{100}, callback, &callback_count);
   EXPECT_NE(h.id, 0u);

   EXPECT_TRUE(driver.cancel(h));
   EXPECT_FALSE(driver.cancel(h));
}

/* ============================================================================
 * Duration Conversion Tests
 * ========================================================================= */

TEST_F(TicklessTimeDriverTest, DurationConversion_Basic)
{
   TicklessDriver driver;

   // These expectations assume your Linux port returns:
   // cortos_port_time_freq_hz() == 1'000'000 (1 tick = 1 us)
   //
   // If you pick a different frequency, adjust the expected values accordingly.

   Duration d1 = driver.from_milliseconds(10);
   EXPECT_EQ(d1.value, 10'000u);

   Duration d2 = driver.from_microseconds(5000);
   EXPECT_EQ(d2.value, 5000u);

   // Rounding up: 1001us -> 1001 ticks at 1MHz (no rounding needed)
   Duration d3 = driver.from_microseconds(1001);
   EXPECT_EQ(d3.value, 1001u);
}

/* ============================================================================
 * Slot Exhaustion Tests
 * ========================================================================= */

TEST_F(TicklessTimeDriverTest, MaxScheduledCallbacks)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);
   driver.start();

   std::atomic<int> callback_count{0};
   auto callback = [](void* arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
   };

   std::vector<ITimeDriver::Handle> handles;
   handles.reserve(TicklessDriver::MAX_SCHEDULED_CALLBACKS);

   for (uint32_t i = 0; i < TicklessDriver::MAX_SCHEDULED_CALLBACKS; ++i) {
      auto h = driver.schedule_at(TimePoint{static_cast<uint64_t>(i + 10)}, callback, &callback_count);
      EXPECT_NE(h.id, 0u);
      handles.push_back(h);
   }

   // Next should fail
   auto overflow = driver.schedule_at(TimePoint{999}, callback, &callback_count);
   EXPECT_EQ(overflow.id, 0u);

   // Free one
   EXPECT_TRUE(driver.cancel(handles[0]));

   // Now should succeed
   auto again = driver.schedule_at(TimePoint{999}, callback, &callback_count);
   EXPECT_NE(again.id, 0u);
}

/* ============================================================================
 * Start/Stop Tests
 * ========================================================================= */

TEST_F(TicklessTimeDriverTest, StartStop)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);

   driver.start();

   cortos_port_time_advance(123);
   driver.on_timer_isr();
   EXPECT_EQ(driver.now().value, 123u);

   driver.stop();

   // Can restart
   driver.start();

   cortos_port_time_advance(10);
   driver.on_timer_isr();
   EXPECT_EQ(driver.now().value, 133u);
}

TEST_F(TicklessTimeDriverTest, SingletonAccess)
{
   TicklessDriver driver;
   ITimeDriver::set_instance(&driver);

   ITimeDriver& instance = ITimeDriver::get_instance();
   EXPECT_EQ(&instance, &driver);

   // Deterministic start (SetUp reset already called)
   EXPECT_EQ(instance.now().value, 0u);
}
