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
 * Global state
 * ========================================================================= */
struct GlobalState
{
   cortos_port_reschedule_t reschedule_cb{nullptr};
};
[[maybe_unused]]
static constinit GlobalState global;

/* ============================================================================
 * Thread-Local State (Per OS thread, NOT boost fiber)
 * For SMP simulation, each OS thread has its own state tracking using thread_local
 * ========================================================================= */
struct CurrentCoreState
{
   uint32_t core_id{0};
   cortos_port_context* current_context{nullptr};
   // The "caller" fiber for the currently running task on *this OS thread*.
   // This is NOT stored in the cortos_port_context, so it won't migrate.
   boost::context::fiber task_caller;
   void* tls_pointer{nullptr}; // Simulates pointing to fibers dedicated TLS block.
};
static thread_local constinit CurrentCoreState current_core;

/* ============================================================================
 * Core Identification
 * ========================================================================= */

extern "C" uint32_t cortos_port_get_core_id(void)
{
   return current_core.core_id;
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
   ::new (context) cortos_port_context{
      .thread     = {},
      .stack_top  = static_cast<uint8_t*>(stack_base) + stack_size,
      .stack_size = stack_size,
      .entry      = entry,
      .arg        = arg,
   };

   // Build a fiber bound to the user-provided stack
   boost::context::stack_context boost_stack_context{
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
      [context](boost::context::fiber&& caller) mutable -> boost::context::fiber
      {
         // Store scheduler continuation so we can jump back
         current_core.task_caller     = std::move(caller);
         current_core.current_context = context;

         try {
            context->entry(context->arg); // Enter user code
         } catch (boost::context::detail::forced_unwind const&) {
            current_core.current_context = nullptr;
            throw;
         }
         current_core.current_context = nullptr;

         return std::move(current_core.task_caller);
      }
   );
}


extern "C" void cortos_port_pend_reschedule(void)
{
   // Only meaningful if we're currently inside a task fiber on this OS thread.
   if (!current_core.current_context) return;

   assert(current_core.task_caller && "pend_reschedule: no caller captured");

   // Yield back to the current caller (scheduler/dispatcher).
   // When the scheduler later resumes us again, Boost will pass us a fresh caller,
   // and the lambda above will overwrite tls_task_caller accordingly.
   current_core.task_caller = std::move(current_core.task_caller).resume();
}

extern "C" void cortos_port_context_destroy(cortos_port_context_t* context)
{
   // Verify fiber has completed
   if (context->thread) {
      // Bug: destroying a live thread
      std::abort();
   }

   //context->sched = boost::context::fiber{};
   context->~cortos_port_context();
}

extern "C" void cortos_port_switch(cortos_port_context_t* /*from*/, cortos_port_context_t* to)
{
   assert(to->thread && "No context to switch to");

   current_core.current_context = to;
   to->thread = std::move(to->thread).resume();
   current_core.current_context = nullptr;
}

extern "C" void cortos_port_start_first(cortos_port_context_t* first)
{
   // Nothing special to be done on the first switch
   cortos_port_switch(nullptr, first);
}

extern "C" void cortos_port_thread_exit(void)
{
   cortos_port_pend_reschedule();
   // Should be unreachable?
   std::abort();
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
   if (core_id == current_core.core_id) {
      cortos_port_pend_reschedule();
   }
   // else: TODO when pthread-per-core exists
}

struct BackendCoreThread
{
   pthread_t pthread{};
   uint32_t  core_id{};
   cortos_port_core_entry_t entry{};
};

void cortos_port_start_cores(cortos_port_core_entry_t entry)
{
   // Allocate init blocks on heap so lifetime survives even if start_cores returns.
   auto* threads = new std::array<BackendCoreThread, CORTOS_PORT_CORE_COUNT - 1>{};

   for (uint32_t core_id = 1; auto& thread : *threads) {
      thread.core_id = core_id++;
      thread.entry   = entry;
      pthread_create(
         &thread.pthread,
         nullptr,
         +[](void* arg)-> void*
         {
            auto* init = static_cast<BackendCoreThread*>(arg);
            current_core.core_id = init->core_id;
            init->entry();
            return nullptr;
         },
         &thread
      );
   }

   // Core0 runs on calling thread
   current_core.core_id = 0;
   entry();

   // If entry returns, join children to avoid leaks/dangling behavior
   for (auto& thread : *threads) {
      pthread_join(thread.pthread, nullptr);
   }
   delete threads;
}

// Likely no-op on real targets.
void cortos_port_on_core_returned()
{

}

/* ============================================================================
 * Thread-Local Storage
 * ========================================================================= */

extern "C" void cortos_port_set_tls_pointer(void* tls_base)
{
   current_core.tls_pointer = tls_base;
}

extern "C" void* cortos_port_get_tls_pointer(void)
{
   return current_core.tls_pointer;
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
   //std::printf("Core: %d: cortos_port_idle()\n", current_core.core_id);
   // Sleep 1ms to simulate power saving, then yield
   struct timespec req = {.tv_sec = 0, .tv_nsec = 1'000'000};
   nanosleep(&req, nullptr);
   cortos_port_pend_reschedule();
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

struct TimeState
{
   std::atomic<bool>        irq_enabled{false};
   std::atomic<uint64_t>            now{0};
   std::atomic<uint64_t> armed_deadline{UINT64_MAX};

   std::atomic<cortos_port_isr_handler_t> isr{nullptr};
   std::atomic<void*>                 isr_arg{nullptr};
};
static constinit TimeState time_state;

extern "C" void cortos_port_time_setup(uint32_t tick_hz)
{
   (void)tick_hz;
}

extern "C" uint64_t cortos_port_time_now(void)
{
   return time_state.now.load(std::memory_order_relaxed);
}

extern "C" uint64_t cortos_port_time_freq_hz(void)
{
   return 1'000'000ull; // 1 tick = 1 us (recommend)
}

extern "C" void cortos_port_time_reset(uint64_t t)
{
   time_state.now.store(t, std::memory_order_release);
   time_state.armed_deadline.store(UINT64_MAX, std::memory_order_release);
}

extern "C" void cortos_port_time_register_isr_handler(cortos_port_isr_handler_t h, void* arg)
{
   time_state.isr_arg.store(arg, std::memory_order_relaxed);
   time_state.isr.store(h, std::memory_order_release);
}

extern "C" void cortos_port_time_irq_enable(void)  { time_state.irq_enabled.store(true,  std::memory_order_release); }
extern "C" void cortos_port_time_irq_disable(void) { time_state.irq_enabled.store(false, std::memory_order_release); }

extern "C" void cortos_port_time_arm(uint64_t deadline)
{
   // Keep earliest
   uint64_t cur = time_state.armed_deadline.load(std::memory_order_relaxed);
   while (deadline < cur &&
            !time_state.armed_deadline.compare_exchange_weak(cur, deadline,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed))
   {}
}

extern "C" void cortos_port_time_disarm(void)
{
   time_state.armed_deadline.store(UINT64_MAX, std::memory_order_release);
}

// Linux-only helper for tests
extern "C" void cortos_port_time_advance(uint64_t delta)
{
   time_state.now.fetch_add(delta, std::memory_order_release);
}

extern "C" void cortos_port_send_time_ipi(uint32_t /*core_id*/)
{
   // SMP simulation TODO: poke target core thread.
}
