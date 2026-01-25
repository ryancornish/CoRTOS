/**
 * @file test_port.cpp
 * @brief Unit tests for port layer (boost.context backend)
 */

#include "cortos/port.h"

#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <cstring>

/* ============================================================================
 * Test Fixtures
 * ========================================================================= */

class PortTest : public ::testing::Test
{
protected:
   void SetUp() override
   {
      cortos_port_init();
   }
};

/* ============================================================================
 * Context Switching Tests
 * ========================================================================= */

TEST_F(PortTest, ContextCreationAndDestruction)
{
   // Allocate stack
   constexpr size_t stack_size = 4096;
   std::vector<uint8_t> stack(stack_size);

   // Allocate context using port_traits
   alignas(CORTOS_PORT_CONTEXT_ALIGN) std::array<std::byte, CORTOS_PORT_CONTEXT_SIZE> context_storage{};
   auto* context = reinterpret_cast<cortos_port_context_t*>(context_storage.data());

   bool entry_called = false;

   auto entry = +[](void* arg)
   {
      bool* flag = static_cast<bool*>(arg);
      *flag = true;
   };

   // Initialize context
   cortos_port_context_init(
      context,
      stack.data(),
      stack_size,
      entry,
      &entry_called
   );

   // Run the fiber once
   cortos_port_start_first(context);

   EXPECT_TRUE(entry_called);

   // Destroy context
   cortos_port_context_destroy(context);
}

TEST_F(PortTest, ContextSwitching)
{
   constexpr size_t stack_size = 4096;

   std::vector<uint8_t> stack1(stack_size);
   std::vector<uint8_t> stack2(stack_size);

   alignas(CORTOS_PORT_CONTEXT_ALIGN) std::array<std::byte, CORTOS_PORT_CONTEXT_SIZE> ctx1_storage{};
   alignas(CORTOS_PORT_CONTEXT_ALIGN) std::array<std::byte, CORTOS_PORT_CONTEXT_SIZE> ctx2_storage{};

   auto* ctx1 = reinterpret_cast<cortos_port_context_t*>(ctx1_storage.data());
   auto* ctx2 = reinterpret_cast<cortos_port_context_t*>(ctx2_storage.data());

   auto entry1 = [](void* arg)
   {
      auto* steps = static_cast<int*>(arg);
      steps[0] = ++steps[2];  // step1 = 1
      cortos_port_pend_reschedule();
      steps[0] = ++steps[2];  // step1 = 3
   };

   auto entry2 = [](void* arg)
   {
      auto* steps = static_cast<int*>(arg);
      steps[1] = ++steps[2];  // step2 = 2
      cortos_port_pend_reschedule();
      steps[1] = ++steps[2];  // step2 = 4
   };

   int steps[3] = {0, 0, 0};  // [step1, step2, execution_order]

   cortos_port_context_init(ctx1, stack1.data(), stack_size, entry1, steps);
   cortos_port_context_init(ctx2, stack2.data(), stack_size, entry2, steps);

   // Run thread 1
   cortos_port_start_first(ctx1);
   EXPECT_EQ(steps[0], 1);  // Thread 1 ran first

   // Switch to thread 2
   cortos_port_switch(nullptr, ctx2);
   EXPECT_EQ(steps[1], 2);  // Thread 2 ran second

   // Resume thread 1
   cortos_port_switch(nullptr, ctx1);
   EXPECT_EQ(steps[0], 3);  // Thread 1 resumed

   // Resume thread 2
   cortos_port_switch(nullptr, ctx2);
   EXPECT_EQ(steps[1], 4);  // Thread 2 resumed

   cortos_port_context_destroy(ctx1);
   cortos_port_context_destroy(ctx2);
}

TEST_F(PortTest, YieldReturnsToScheduler)
{
   constexpr size_t stack_size = 4096;
   std::vector<uint8_t> stack(stack_size);

   alignas(CORTOS_PORT_CONTEXT_ALIGN) std::array<std::byte, CORTOS_PORT_CONTEXT_SIZE> context_storage{};
   auto* context = reinterpret_cast<cortos_port_context_t*>(context_storage.data());

   int yield_count = 0;

   auto entry = [](void* arg)
   {
      int* count = static_cast<int*>(arg);
      (*count)++;
      cortos_port_pend_reschedule();
      (*count)++;
      cortos_port_pend_reschedule();
      (*count)++;
   };

   cortos_port_context_init(context, stack.data(), stack_size, entry, &yield_count);

   // First run: executes until first yield
   cortos_port_start_first(context);
   EXPECT_EQ(yield_count, 1);

   // Resume: executes until second yield
   cortos_port_switch(nullptr, context);
   EXPECT_EQ(yield_count, 2);

   // Resume: executes until completion
   cortos_port_switch(nullptr, context);
   EXPECT_EQ(yield_count, 3);

   cortos_port_context_destroy(context);
}

/* ============================================================================
 * TLS Tests
 * ========================================================================= */

TEST_F(PortTest, ThreadLocalStorage)
{
   void* test_ptr = reinterpret_cast<void*>(0xDEADBEEF);

   cortos_port_set_tls_pointer(test_ptr);
   EXPECT_EQ(cortos_port_get_tls_pointer(), test_ptr);

   cortos_port_set_tls_pointer(nullptr);
   EXPECT_EQ(cortos_port_get_tls_pointer(), nullptr);
}


/* ============================================================================
 * Core ID Tests
 * ========================================================================= */

TEST_F(PortTest, CoreIdentification)
{
   uint32_t core_id = cortos_port_get_core_id();
   EXPECT_EQ(core_id, 0);  // Single-threaded test, always core 0
}

/* ============================================================================
 * Interrupt Control Tests
 * ========================================================================= */

TEST_F(PortTest, InterruptControl)
{
   EXPECT_TRUE(cortos_port_interrupts_enabled());

   cortos_port_disable_interrupts();
   EXPECT_FALSE(cortos_port_interrupts_enabled());

   cortos_port_disable_interrupts();  // Nested
   EXPECT_FALSE(cortos_port_interrupts_enabled());

   cortos_port_enable_interrupts();
   EXPECT_FALSE(cortos_port_interrupts_enabled());  // Still disabled (nested)

   cortos_port_enable_interrupts();
   EXPECT_TRUE(cortos_port_interrupts_enabled());  // Now enabled
}

/* ============================================================================
 * Multi-core (start_cores) Tests (configurable cores)
 * ========================================================================= */

struct SpinBarrier
{
   explicit SpinBarrier(uint32_t total_) : total(total_) {}

   void reset(uint32_t total_)
   {
      total = total_;
      arrived.store(0, std::memory_order_relaxed);
      go.store(false, std::memory_order_relaxed);
   }

   void arrive_and_wait()
   {
      uint32_t v = arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (v == total) {
         go.store(true, std::memory_order_release);
      } else {
         while (!go.load(std::memory_order_acquire)) {
            cortos_port_cpu_relax();
         }
      }
   }

   std::atomic<uint32_t> arrived{0};
   std::atomic<bool>     go{false};
   uint32_t total{0};
};

// Global (per TU) state so core-entry can be captureless.
struct StartCoresTestState
{
   std::atomic<bool>     configured{false};
   uint32_t              use_cores{1};

   // Barriers and result arrays.
   SpinBarrier barrier{1};

   std::array<std::atomic<bool>, CORTOS_PORT_CORE_COUNT> seen{};
   std::array<std::atomic<uint32_t>, CORTOS_PORT_CORE_COUNT> observed_core_id{};
   std::array<std::atomic<void*>, CORTOS_PORT_CORE_COUNT> tls_readback{};
   std::array<std::atomic<int>, CORTOS_PORT_CORE_COUNT> step{};

   // To ensure each core runs entry at most once.
   std::array<std::atomic<bool>, CORTOS_PORT_CORE_COUNT> already_reported{};

   void reset(uint32_t cores)
   {
      use_cores = cores;
      barrier.reset(cores);

      for (uint32_t i = 0; i < CORTOS_PORT_CORE_COUNT; ++i) {
         seen[i].store(false, std::memory_order_relaxed);
         observed_core_id[i].store(0xFFFFFFFFu, std::memory_order_relaxed);
         tls_readback[i].store(nullptr, std::memory_order_relaxed);
         step[i].store(0, std::memory_order_relaxed);
         already_reported[i].store(false, std::memory_order_relaxed);
      }

      configured.store(true, std::memory_order_release);
   }
};

static StartCoresTestState g;

// ----- captureless core entries -----

static void core_entry_seen_and_coreid()
{
   // Wait until main thread configured state (paranoia for racing starts).
   while (!g.configured.load(std::memory_order_acquire)) {
      cortos_port_cpu_relax();
   }

   const uint32_t cid = cortos_port_get_core_id();
   ASSERT_LT(cid, CORTOS_PORT_CORE_COUNT);

   // Only cores [0..use_cores-1] should run entry.
   ASSERT_LT(cid, g.use_cores) << "Port ran entry on an unexpected core";

   bool expected = false;
   ASSERT_TRUE(g.already_reported[cid].compare_exchange_strong(expected, true,
                                                              std::memory_order_acq_rel))
      << "Core entry ran more than once on core " << cid;

   cortos_port_set_tls_pointer(reinterpret_cast<void*>(uintptr_t(0x1000u + cid)));

   // Sync participating cores.
   g.barrier.arrive_and_wait();

   g.observed_core_id[cid].store(cid, std::memory_order_release);
   g.seen[cid].store(true, std::memory_order_release);
}

static void core_entry_tls_is_per_core()
{
   while (!g.configured.load(std::memory_order_acquire)) {
      cortos_port_cpu_relax();
   }

   const uint32_t cid = cortos_port_get_core_id();
   ASSERT_LT(cid, CORTOS_PORT_CORE_COUNT);
   ASSERT_LT(cid, g.use_cores);

   void* unique = reinterpret_cast<void*>(uintptr_t(0xDEAD0000u + cid));
   cortos_port_set_tls_pointer(unique);

   g.barrier.arrive_and_wait();

   void* rb = cortos_port_get_tls_pointer();
   g.tls_readback[cid].store(rb, std::memory_order_release);
}

static void core_entry_pend_and_self_ipi_yield()
{
   while (!g.configured.load(std::memory_order_acquire)) {
      cortos_port_cpu_relax();
   }

   const uint32_t cid = cortos_port_get_core_id();
   ASSERT_LT(cid, CORTOS_PORT_CORE_COUNT);
   ASSERT_LT(cid, g.use_cores);

   constexpr size_t stack_size = 4096;
   auto stack = std::make_unique<std::uint8_t[]>(stack_size);

   alignas(CORTOS_PORT_CONTEXT_ALIGN) std::array<std::byte, CORTOS_PORT_CONTEXT_SIZE> ctx_storage{};
   auto* ctx = reinterpret_cast<cortos_port_context_t*>(ctx_storage.data());

   auto task_entry = +[](void* arg)
   {
      auto* s = static_cast<std::atomic<int>*>(arg);

      s->store(1, std::memory_order_release);
      cortos_port_pend_reschedule();

      s->store(2, std::memory_order_release);
      cortos_port_send_reschedule_ipi(cortos_port_get_core_id());

      s->store(3, std::memory_order_release);
   };

   cortos_port_context_init(ctx, stack.get(), stack_size, task_entry, &g.step[cid]);

   cortos_port_start_first(ctx);
   EXPECT_EQ(g.step[cid].load(std::memory_order_acquire), 1);

   cortos_port_switch(nullptr, ctx);
   EXPECT_EQ(g.step[cid].load(std::memory_order_acquire), 2);

   cortos_port_switch(nullptr, ctx);
   EXPECT_EQ(g.step[cid].load(std::memory_order_acquire), 3);

   cortos_port_context_destroy(ctx);

   g.barrier.arrive_and_wait();
}

TEST_F(PortTest, StartCores_UsesConfiguredCoreCountAndSetsCoreId)
{
   // Pick something representative: if HW has >=2 cores, use 2, else 1.
   const uint32_t use = (CORTOS_PORT_CORE_COUNT >= 2) ? 2u : 1u;

   g.reset(use);

   // New signature: (cores_to_use, entry)
   cortos_port_start_cores(use, &core_entry_seen_and_coreid);

   // Verify only [0..use-1] participated.
   for (uint32_t i = 0; i < use; ++i) {
      EXPECT_TRUE(g.seen[i].load(std::memory_order_acquire)) << "Core " << i << " did not run entry";
      EXPECT_EQ(g.observed_core_id[i].load(std::memory_order_acquire), i);
   }
   for (uint32_t i = use; i < CORTOS_PORT_CORE_COUNT; ++i) {
      EXPECT_FALSE(g.seen[i].load(std::memory_order_acquire)) << "Unexpected core " << i << " ran entry";
   }
}

TEST_F(PortTest, StartCores_TlsPointerIsPerCoreThread)
{
   const uint32_t use = (CORTOS_PORT_CORE_COUNT >= 2) ? 2u : 1u;

   g.reset(use);

   cortos_port_start_cores(use, &core_entry_tls_is_per_core);

   for (uint32_t i = 0; i < use; ++i) {
      void* expected = reinterpret_cast<void*>(uintptr_t(0xDEAD0000u + i));
      EXPECT_EQ(g.tls_readback[i].load(std::memory_order_acquire), expected)
         << "TLS leaked or core-local TLS broken on core " << i;
   }
   for (uint32_t i = use; i < CORTOS_PORT_CORE_COUNT; ++i) {
      EXPECT_EQ(g.tls_readback[i].load(std::memory_order_acquire), nullptr)
         << "Non-participating core " << i << " should not have written TLS readback";
   }
}

TEST_F(PortTest, StartCores_PendRescheduleAndSelfIpiWorkPerCore)
{
   const uint32_t use = (CORTOS_PORT_CORE_COUNT >= 2) ? 2u : 1u;

   g.reset(use);

   cortos_port_start_cores(use, &core_entry_pend_and_self_ipi_yield);

   for (uint32_t i = 0; i < use; ++i) {
      EXPECT_EQ(g.step[i].load(std::memory_order_acquire), 3)
         << "Core " << i << " did not complete yield/resume sequence";
   }
   for (uint32_t i = use; i < CORTOS_PORT_CORE_COUNT; ++i) {
      EXPECT_EQ(g.step[i].load(std::memory_order_acquire), 0)
         << "Non-participating core " << i << " unexpectedly ran";
   }
}

TEST_F(PortTest, Asserts)
{
   CORTOS_ASSERT(true);
   CORTOS_ASSERT1(true == true, 30);
   CORTOS_ASSERT2(!false, 30, 20);
   CORTOS_ASSERT_OP(1, <, 2);
   int* i_am_null = nullptr;
   CORTOS_ASSERT_NULL(i_am_null);
}

// Uncomment each line to test kernel panics
TEST_F(PortTest, MakesError)
{
   // CORTOS_ASSERT(false); // ...Some handy error diagnosis comment...
   // CORTOS_ASSERT1(true == false, 30);
   // CORTOS_ASSERT2(!true, 30, 20); // ...Some handy error diagnosis comment...
   // CORTOS_ASSERT_OP(1, >, 2);  // ...Some handy error diagnosis comment...
   // int x; CORTOS_ASSERT_NULL(&x);  // ...Some handy error diagnosis comment...
}

