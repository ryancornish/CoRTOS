/**
 * @file port_linux_boost.cpp
 * @brief Linux simulation port using Boost.Context
 *
 * This port uses Boost.Context for fast cooperative context switching.
 * It simulates embedded behavior (stack-based context switching) while
 * running on Linux for development and testing.
 *
 * SMP support: Each pthread represents a "core". Use cortos_port_get_core_id()
 * to determine which simulated core is running.
 */

#include "cortos/port.h"
#include "port_traits.h"

#include <algorithm>
#include <boost/context/fiber.hpp>
#include <boost/context/preallocated.hpp>
#include <boost/context/stack_context.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <time.h>

/* ============================================================================
 * Port Context Structure
 * ========================================================================= */

struct cortos_port_context
{
   boost::context::fiber thread;  // Thread fiber (owned by scheduler when idle)
   boost::context::fiber sched;   // Scheduler fiber (owned by thread when running)
   void*                 stack_top;
   size_t                stack_size;
   cortos_port_entry_t   entry;
   void*                 arg;
};

// Verify that port_traits.h constants are correct
static_assert(sizeof(cortos_port_context) == CORTOS_PORT_CONTEXT_SIZE,
              "CORTOS_PORT_CONTEXT_SIZE mismatch - adjust in port_traits.h");
static_assert(alignof(cortos_port_context) == CORTOS_PORT_CONTEXT_ALIGN,
              "CORTOS_PORT_CONTEXT_ALIGN mismatch - adjust in port_traits.h");
static_assert((CORTOS_STACK_ALIGN & (CORTOS_STACK_ALIGN - 1)) == 0,
              "CORTOS_STACK_ALIGN must be a power of two");

/* ============================================================================
 * Thread-Local State
 * ========================================================================= */

// Current thread context (used by port_yield)
static thread_local cortos_port_context* tls_current_context = nullptr;

// TLS pointer (simulates hardware TLS register)
static thread_local void* tls_thread_pointer = nullptr;

// Simulated core ID (set when pthread is created for SMP simulation)
static thread_local uint32_t tls_core_id = 0;

static thread_local boost::context::fiber tls_sched_fiber{};
static thread_local bool tls_in_scheduler = false;
static thread_local std::atomic<bool> tls_pendsv_pending{false}; // per-core pending flag


/* ============================================================================
 * Core Identification
 * ========================================================================= */

extern "C" uint32_t cortos_port_get_core_id(void)
{
   return tls_core_id;
}

/* ============================================================================
 * Context Switching
 * ========================================================================= */

// No-op stack allocator for preallocated memory
struct preallocated_stack_noop
{
   using traits_type = boost::context::stack_traits;
   boost::context::stack_context allocate(size_t) { std::abort(); }
   void deallocate(boost::context::stack_context&) noexcept {}
};

extern "C" void cortos_port_context_init(cortos_port_context_t* context,
                                  void* stack_base,
                                  size_t stack_size,
                                  cortos_port_entry_t entry,
                                  void* arg)
{
   // Construct cortos_port_context_t in place
   ::new (context) cortos_port_context
   {
      .thread     = {},
      .sched      = {},
      .stack_top  = static_cast<uint8_t*>(stack_base) + stack_size,
      .stack_size = stack_size,
      .entry      = entry,
      .arg        = arg,
   };

   // Build a fiber bound to the user-provided stack
   boost::context::stack_context boost_stack_context =
   {
      .size = context->stack_size,
      .sp   = context->stack_top,
   };

   boost::context::preallocated boost_prealloc(
      boost_stack_context.sp,
      boost_stack_context.size,
      boost_stack_context
   );

   preallocated_stack_noop stack_allocator;

   context->thread = boost::context::fiber(
      std::allocator_arg,
      boost_prealloc,
      stack_allocator,
      [context](boost::context::fiber&& sched_in) mutable -> boost::context::fiber
      {
         // Store scheduler continuation so port_yield() can jump back
         context->sched = std::move(sched_in);

         try {
            tls_current_context = context;
            context->entry(context->arg); // Enter user code
            tls_current_context = nullptr;
         } catch (boost::context::detail::forced_unwind const& x) {
            tls_current_context = nullptr;
            throw x;
         }

         return std::move(context->sched);
      }
   );
}

extern "C" void cortos_port_run_scheduler(void (*entry)(void*), void* arg)
{
   // Construct scheduler fiber that never returns
   tls_sched_fiber = boost::context::fiber([=](boost::context::fiber&& caller) mutable {
      (void)caller;
      tls_in_scheduler = true;

      // "entry" is your kernel scheduler loop for this core
      entry(arg);

      std::abort(); // must never return
      return boost::context::fiber{};
   });

   // Enter scheduler fiber
   tls_sched_fiber = std::move(tls_sched_fiber).resume();
   std::abort();
}

extern "C" void cortos_port_pend_reschedule(void)
{
   // Mark pending (coalesce)
   tls_pendsv_pending.store(true, std::memory_order_release);

   // If we are in thread context, yield immediately to the scheduler fiber.
   // If we are already in scheduler context, do nothing (it will observe pending).
   if (!tls_in_scheduler) {
      cortos_port_yield();
   }
}


extern "C" void cortos_port_context_destroy(cortos_port_context_t* context)
{
   // Verify fiber has completed
   if (context->thread) {
      // Bug: destroying a live thread
      std::abort();
   }

   context->sched = boost::context::fiber{};
   context->~cortos_port_context();
}

extern "C" void cortos_port_switch(cortos_port_context_t* /*from*/, cortos_port_context_t* to)
{
   assert(to->thread && "No context to switch to");

   tls_current_context = to;
   to->thread = std::move(to->thread).resume();
   tls_current_context = nullptr;
}

extern "C" void cortos_port_start_first(cortos_port_context_t* first)
{
   tls_current_context = first;
   first->thread = std::move(first->thread).resume();
   tls_current_context = nullptr;
}

extern "C" void cortos_port_yield(void)
{
   // No current context - nothing to yield from
   if (!tls_current_context) return;

   auto* current = tls_current_context;
   tls_current_context = nullptr;

   assert(current->sched && "No scheduler context to switch to");
   current->sched = std::move(current->sched).resume();
}

extern "C" void cortos_port_thread_exit(void)
{
   // Boost.Context cleans up automatically when fiber exits
   // Just ensure we don't return
   while (true) {
      cortos_port_yield();
   }
}

/* ============================================================================
 * Critical Sections (Simulated)
 * ========================================================================= */

static thread_local uint32_t interrupt_disable_depth = 0;

extern "C" void cortos_port_disable_interrupts(void)
{
   interrupt_disable_depth++;
}

extern "C" void cortos_port_enable_interrupts(void)
{
   if (interrupt_disable_depth > 0) {
      interrupt_disable_depth--;
   }
}

extern "C" bool cortos_port_interrupts_enabled(void)
{
   return interrupt_disable_depth == 0;
}

extern "C" uint32_t cortos_port_irq_save(void)
{
   // Return previous enabled-state as 1/0 (simple)
   uint32_t prev_enabled = (interrupt_disable_depth == 0) ? 1u : 0u;
   interrupt_disable_depth++;
   return prev_enabled;
}

extern "C" void cortos_port_irq_restore(uint32_t state)
{
   // Unwind one nesting level
   if (interrupt_disable_depth > 0) {
      interrupt_disable_depth--;
   }
}


/* ============================================================================
 * CPU Hints
 * ========================================================================= */

extern "C" void cortos_port_cpu_relax(void)
{
   // CPU yield hint for busy-wait loops
#if defined(__x86_64__) || defined(__i386__)
   __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
   __asm__ __volatile__("yield");
#endif
}

/* ============================================================================
 * Inter-Processor Interrupts (SMP Simulation)
 * ========================================================================= */

extern "C" void cortos_port_send_reschedule_ipi(uint32_t core_id)
{
   // TODO: For SMP simulation, signal the pthread representing core_id
   // For now, this is a no-op
   (void)core_id;
}

/* ============================================================================
 * Thread-Local Storage
 * ========================================================================= */

extern "C" void cortos_port_set_tls_pointer(void* tls_base)
{
   tls_thread_pointer = tls_base;
}

extern "C" void* cortos_port_get_tls_pointer(void)
{
   return tls_thread_pointer;
}

/* ============================================================================
 * Platform Initialization
 * ========================================================================= */

extern "C" void cortos_port_init(void)
{
}

/* ============================================================================
 * Idle Hook
 * ========================================================================= */

extern "C" void cortos_port_idle(void)
{
   // Sleep 1ms to simulate power saving, then yield
   struct timespec req = {.tv_sec = 0, .tv_nsec = 1'000'000};
   nanosleep(&req, nullptr);
   cortos_port_yield();
}

/* ============================================================================
 * Debug / Diagnostics
 * ========================================================================= */

extern "C" void cortos_port_breakpoint(void)
{
#if defined(__x86_64__) || defined(__i386__)
   __asm__ __volatile__("int3");
#elif defined(__aarch64__) || defined(__arm__)
   __builtin_trap();
#else
   raise(SIGTRAP);
#endif
}

extern "C" void* cortos_port_get_stack_pointer(void)
{
   void* sp;
#if defined(__x86_64__)
   __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp));
#elif defined(__i386__)
   __asm__ __volatile__("mov %%esp, %0" : "=r"(sp));
#elif defined(__aarch64__)
   __asm__ __volatile__("mov %0, sp" : "=r"(sp));
#elif defined(__arm__)
   __asm__ __volatile__("mov %0, sp" : "=r"(sp));
#else
   int dummy;
   sp = &dummy;
#endif
   return sp;
}

/* ============================================================================
 * Time Driver Port (Linux Boost)
 *
 * Purpose:
 * - Provide monotonic time for real drivers (periodic / tickless) in unit tests
 * - Provide tickless one-shot arming and ISR delivery when pumped
 *
 * Note:
 * - SimulationTimeDriver owns time and does NOT use this.
 * - Periodic driver unit tests call driver.on_timer_isr() directly
 * ========================================================================= */

static std::atomic<uint64_t> g_port_now{0};

static std::atomic<bool> g_time_irq_enabled{false};
static std::atomic<uint64_t> g_armed_deadline{UINT64_MAX};
static std::atomic<cortos_port_isr_handler_t> g_isr{nullptr};
static std::atomic<void*> g_isr_arg{nullptr};

extern "C" void cortos_port_time_setup(uint32_t tick_hz)
{
   (void)tick_hz;
}

extern "C" uint64_t cortos_port_time_now(void)
{
   return g_port_now.load(std::memory_order_relaxed);
}

extern "C" uint64_t cortos_port_time_freq_hz(void)
{
   return 1'000'000ull; // 1 tick = 1 us (recommend)
}

extern "C" void cortos_port_time_reset(uint64_t t)
{
   g_port_now.store(t, std::memory_order_release);
   g_armed_deadline.store(UINT64_MAX, std::memory_order_release);
}

extern "C" void cortos_port_time_register_isr_handler(cortos_port_isr_handler_t h, void* arg)
{
   g_isr_arg.store(arg, std::memory_order_relaxed);
   g_isr.store(h, std::memory_order_release);
}

extern "C" void cortos_port_time_irq_enable(void)  { g_time_irq_enabled.store(true,  std::memory_order_release); }
extern "C" void cortos_port_time_irq_disable(void) { g_time_irq_enabled.store(false, std::memory_order_release); }

extern "C" void cortos_port_time_arm(uint64_t deadline)
{
   // Keep earliest
   uint64_t cur = g_armed_deadline.load(std::memory_order_relaxed);
   while (deadline < cur &&
            !g_armed_deadline.compare_exchange_weak(cur, deadline,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed))
   {}
}

extern "C" void cortos_port_time_disarm(void)
{
   g_armed_deadline.store(UINT64_MAX, std::memory_order_release);
}

// Linux-only helper for tests
extern "C" void cortos_port_time_advance(uint64_t delta)
{
   g_port_now.fetch_add(delta, std::memory_order_release);
}

extern "C" void cortos_port_send_time_ipi(uint32_t /*core_id*/)
{
   // SMP simulation TODO: poke target core thread.
}
