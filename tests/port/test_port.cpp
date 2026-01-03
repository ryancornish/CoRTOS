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
      cortos_port_init(1000);  // 1kHz tick rate
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
   alignas(CORTOS_PORT_CONTEXT_ALIGN) uint8_t context_storage[CORTOS_PORT_CONTEXT_SIZE];
   auto* context = reinterpret_cast<cortos_port_context_t*>(context_storage);

   bool entry_called = false;

   auto entry = [](void* arg)
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

   alignas(CORTOS_PORT_CONTEXT_ALIGN) uint8_t ctx1_storage[CORTOS_PORT_CONTEXT_SIZE];
   alignas(CORTOS_PORT_CONTEXT_ALIGN) uint8_t ctx2_storage[CORTOS_PORT_CONTEXT_SIZE];

   auto* ctx1 = reinterpret_cast<cortos_port_context_t*>(ctx1_storage);
   auto* ctx2 = reinterpret_cast<cortos_port_context_t*>(ctx2_storage);

   auto entry1 = [](void* arg)
   {
      auto* steps = static_cast<int*>(arg);
      steps[0] = ++steps[2];  // step1 = 1
      cortos_port_yield();
      steps[0] = ++steps[2];  // step1 = 3
   };

   auto entry2 = [](void* arg)
   {
      auto* steps = static_cast<int*>(arg);
      steps[1] = ++steps[2];  // step2 = 2
      cortos_port_yield();
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

   alignas(CORTOS_PORT_CONTEXT_ALIGN) uint8_t context_storage[CORTOS_PORT_CONTEXT_SIZE];
   auto* context = reinterpret_cast<cortos_port_context_t*>(context_storage);

   int yield_count = 0;

   auto entry = [](void* arg)
   {
      int* count = static_cast<int*>(arg);
      (*count)++;
      cortos_port_yield();
      (*count)++;
      cortos_port_yield();
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

   uint32_t core_count = cortos_port_get_core_count();
   EXPECT_GE(core_count, 1);
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
