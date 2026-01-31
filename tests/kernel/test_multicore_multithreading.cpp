// test_multicore.cpp
#include "cortos/kernel.hpp"
#include "gtest/gtest.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace cortos;

int main(int argc, char** argv)
{
   ::testing::InitGoogleTest(&argc, argv);

   int result = RUN_ALL_TESTS();

   return result;
}


class MultiCoreMultiThread_Test : public ::testing::Test
{
   void SetUp() override
   {
      if (kernel::core_count() < 2)  GTEST_SKIP() << "Need at least 2 cores for these test";
      kernel::initialise();
   }

   void TearDown() override
   {
      kernel::finalise();
   }
};


/* ============================================================================
 * Multicore tests
 * ========================================================================= */

TEST_F(MultiCoreMultiThread_Test,
     GivenTwoCoresAndOneThreadPinnedToEach_WhenKernelStarts_ThenBothThreadsRunOnExpectedCore)
{
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s0{};
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s1{};

   std::atomic<bool> ran0{false};
   std::atomic<bool> ran1{false};
   std::atomic<std::uint32_t> seen_core0{0xFFFFFFFFu};
   std::atomic<std::uint32_t> seen_core1{0xFFFFFFFFu};

   Thread t0(
      [&]{
         seen_core0.store(this_thread::core_id(), std::memory_order_release);
         ran0.store(true, std::memory_order_release);
      },
      s0,
      Thread::Priority(0),
      Core0
   );

   Thread t1(
      [&]{
         seen_core1.store(this_thread::core_id(), std::memory_order_release);
         ran1.store(true, std::memory_order_release);
      },
      s1,
      Thread::Priority(0),
      Core1
   );

   EXPECT_EQ(kernel::active_threads(), 2u);

   kernel::start();

   EXPECT_EQ(kernel::active_threads(), 0u);
   EXPECT_TRUE(ran0.load(std::memory_order_acquire));
   EXPECT_TRUE(ran1.load(std::memory_order_acquire));

   EXPECT_EQ(seen_core0.load(std::memory_order_acquire), 0u);
   EXPECT_EQ(seen_core1.load(std::memory_order_acquire), 1u);
}


TEST_F(MultiCoreMultiThread_Test,
     GivenTwoCores_WhenCore0CreatesAThreadPinnedToCore1AfterStart_ThenCore1RunsIt)
{
   // Stacks must outlive kernel::start() execution.
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s_creator{};
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s_remote{};

   std::atomic<bool> remote_ran{false};
   std::atomic<std::uint32_t> remote_seen_core{0xFFFFFFFFu};

   // Thread on Core0 that creates a new thread pinned to Core1 *after* system is running.
   Thread creator(
      [&]{
         // Ensure we're actually on core0.
         EXPECT_EQ(this_thread::core_id(), 0u);

         // Create a thread pinned to Core1 while running: this should go via
         // Scheduler::post_to_inbox() + cortos_port_send_reschedule_ipi(1).
         Thread remote(
            [&]{
               remote_seen_core.store(this_thread::core_id(), std::memory_order_release);
               remote_ran.store(true, std::memory_order_release);
            },
            s_remote,
            Thread::Priority(0),
            Core1
         );

         // Yield to allow Core1 to be poked / drain inbox / run remote task.
         // In cooperative sim this is necessary.
         for (int i = 0; i < 5; ++i) this_thread::yield();

         // Keep creator alive long enough for remote to run, then return.
      },
      s_creator,
      Thread::Priority(0),
      Core0
   );

   kernel::start();

   EXPECT_TRUE(remote_ran.load(std::memory_order_acquire))
      << "Remote thread never ran (possible missing inbox poke / IPI / idle wake)";
   EXPECT_EQ(remote_seen_core.load(std::memory_order_acquire), 1u);
}

TEST_F(MultiCoreMultiThread_Test,
     GivenUpToFourCoresWithOneThreadEach_WhenKernelStarts_ThenAllCoresMakeProgress)
{
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s0{};
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s1{};
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s2{};
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s3{};

   std::array<std::atomic<int>, 4> stages{};
   for (auto& a : stages) a.store(0, std::memory_order_relaxed);

   auto make_thread = [&](std::uint32_t cid, std::array<std::byte, 16 * 1024>& st)
   {
      Thread(
         [&, cid]{
            // stage 1: started
            stages[cid].store(1, std::memory_order_release);

            // Do some cooperative stepping so reschedule/rotation is exercised.
            for (int i = 0; i < 3; ++i) {
               this_thread::yield();
            }

            // stage 2: finished
            stages[cid].store(2, std::memory_order_release);
         },
         st,
         Thread::Priority(0),
         CoreAffinity::from_id(cid)
      );
   };

   make_thread(0, s0);
   make_thread(1, s1);
   make_thread(2, s2);
   make_thread(3, s3);

   kernel::start();

   for (std::uint32_t cid = 0; cid < kernel::core_count(); ++cid) {
      EXPECT_EQ(stages[cid].load(std::memory_order_acquire), 2)
         << "Core " << cid << " did not complete its thread";
   }
   EXPECT_EQ(kernel::active_threads(), 0u);
}

TEST_F(MultiCoreMultiThread_Test,
     GivenTwoCores_WhenCore0PokesCore1WhileCore1IsIdle_ThenCore1WakesAndRunsQueuedWork)
{
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s_core0{};
   alignas(CORTOS_PORT_STACK_ALIGN) std::array<std::byte, 16 * 1024> s_core1_work{};

   std::atomic<bool> core1_work_ran{false};

   // Make Core1 have *no* initial tasks queued pre-start by creating the core1 work post-start.
   // Core1 will start in idle unless/until it receives inbox work + IPI.
   Thread core0(
      [&]{
         EXPECT_EQ(this_thread::core_id(), 0u);

         // Give Core1 a chance to enter idle first (cooperative).
         for (int i = 0; i < 3; ++i) this_thread::yield();

         Thread t_on_1(
            [&]{
               core1_work_ran.store(true, std::memory_order_release);
            },
            s_core1_work,
            Thread::Priority(0),
            Core1
         );

         // Yield to allow IPI -> idle wake -> inbox drain -> task run.
         for (int i = 0; i < 10; ++i) this_thread::yield();
      },
      s_core0,
      Thread::Priority(0),
      Core0
   );

   kernel::start();

   EXPECT_TRUE(core1_work_ran.load(std::memory_order_acquire))
      << "Core1 did not wake from idle to run queued work (possible missing condvar poke)";
}
