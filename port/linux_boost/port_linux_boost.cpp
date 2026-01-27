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
#include <ctime>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <string>

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
static_assert((CORTOS_PORT_STACK_ALIGN & (CORTOS_PORT_STACK_ALIGN - 1)) == 0,
              "CORTOS_PORT_STACK_ALIGN must be a power of two");
static_assert(CORTOS_PORT_SCHEDULING_TYPE == CORTOS_PORT_SCHED_COOPERATIVE);
static_assert(CORTOS_PORT_ENVIRONMENT == CORTOS_PORT_ENV_SIMULATION);

/* ============================================================================
 * Global & Thread-Local State
 *
 * For SMP simulation, each OS thread has its own state tracking using
 * thread_local. This is NOT stored in cortos_port_context to prevent
 * migration issues.
 * ========================================================================= */

struct BackendCore
{
   pthread_t pthread{}; // NOTE: This is null/unused for Core0
   uint32_t  core_id{};
   cortos_port_core_entry_t entry{};

   struct CorePoke
   {
      pthread_mutex_t mutex{};
      pthread_cond_t  cond_var{};
      std::atomic<bool> pending{false}; // Can be set by any core
      CorePoke()  { pthread_mutex_init(&mutex, nullptr); pthread_cond_init(&cond_var, nullptr); }
      ~CorePoke() { pthread_cond_destroy(&cond_var); pthread_mutex_destroy(&mutex); }
   } core_poke;
};

struct GlobalState
{
   size_t cores_running{0};
   std::span<BackendCore> core_view;
   void reset() { cores_running = 0; core_view = {}; }
};
[[maybe_unused]]
static constinit GlobalState global;

struct CurrentCoreState
{
   uint32_t core_id{0};
   cortos_port_context* current_context{nullptr};
   // The "caller" fiber for the currently running task on *this OS thread*.
   boost::context::fiber task_caller;
   void* tls_pointer{nullptr}; // Simulates pointing to fibers dedicated TLS block.
};
static thread_local constinit CurrentCoreState current_core;

/* ============================================================================
 * Platform Initialization
 * ========================================================================= */

extern "C" void cortos_port_init(void)
{

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
 * Context Management & Switching
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

extern "C" void cortos_port_thread_exit(void)
{
   // Do nothing under boost. This will return then return from the thread_launcher, which will
   // switch to the sched context and this thread context will no longer be captured, and be cleaned up.
}

/* ============================================================================
 * SMP & Multi-Core Support
 *
 * Each pthread represents a simulated "core". Core 0 runs on the calling
 * thread, additional cores spawn as pthreads.
 * ========================================================================= */

extern "C" uint32_t cortos_port_get_core_id(void)
{
   return current_core.core_id;
}

void cortos_port_start_cores(size_t cores_to_use, cortos_port_core_entry_t entry)
{
   CORTOS_ASSERT(cores_to_use > 0); // Invoking with 0 cores_to_use is invalid
   CORTOS_ASSERT(cores_to_use <= CORTOS_PORT_CORE_COUNT);

   global.cores_running = cores_to_use;

   auto cores = std::make_unique<BackendCore[]>(cores_to_use);
   global.core_view = {cores.get(), cores_to_use};
    // No need to spawn the first core/thread as that is assigned to this current calling core/thread
   auto cores_to_spawn = global.core_view.subspan(1);

   for (uint32_t core_id = 1; auto& core : cores_to_spawn) {
      core.core_id = core_id++;
      core.entry   = entry;
      pthread_create(
         &core.pthread,
         nullptr,
         +[](void* arg)-> void*
         {
            auto* init = static_cast<BackendCore*>(arg);
            current_core.core_id = init->core_id;
            init->entry();
            return nullptr;
         },
         &core
      );
   }

   // Core0 runs on calling thread
   current_core.core_id = 0;
   entry();

   // If entry returns, join children to avoid leaks/dangling behavior
   for (auto& core : cores_to_spawn) {
      pthread_join(core.pthread, nullptr);
   }
   global.reset();
}

extern "C" void cortos_port_send_reschedule_ipi(uint32_t core_id)
{
   CORTOS_ASSERT_OP(core_id, <, global.cores_running);

   auto& core_poker = global.core_view[core_id].core_poke;

   // Set the pending bit first (release) so the woken core sees it.
   core_poker.pending.store(true, std::memory_order_release);

   // Wake the core if it is blocked in idle().
   pthread_mutex_lock(&core_poker.mutex);
   pthread_cond_signal(&core_poker.cond_var);
   pthread_mutex_unlock(&core_poker.mutex);

   if (core_id == current_core.core_id) {
      cortos_port_pend_reschedule();
   }
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
 * Time Driver Port
 *
 * Provides monotonic time for real drivers (periodic / tickless) in unit
 * tests, plus tickless one-shot arming and ISR delivery when pumped.
 *
 * Note:
 * - SimulationTimeDriver owns time and does NOT use this.
 * - Periodic driver unit tests call driver.on_timer_isr() directly.
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

/* ============================================================================
 * CPU Hints & Idle
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

extern "C" void cortos_port_idle(void)
{
   std::printf("Core: %d: cortos_port_idle()\n", current_core.core_id);
   auto& core_poker = global.core_view[current_core.core_id].core_poke;

   // Fast path: don’t sleep if already pending.
   if (core_poker.pending.exchange(false, std::memory_order_acq_rel)) {
      return;
   }

   pthread_mutex_lock(&core_poker.mutex);

   // Re-check under lock (avoids missed wake if signal happens between fast-path and lock)
   while (!core_poker.pending.exchange(false, std::memory_order_acq_rel)) {
      pthread_cond_wait(&core_poker.cond_var, &core_poker.mutex);
   }

   pthread_mutex_unlock(&core_poker.mutex);
}

/* ============================================================================
 * Debug & Diagnostics
 * ========================================================================= */

static void print_formatted_context(char const* file, int target_line, int range = 2)
{
   // Colour Constants
   static constexpr auto CLR_RESET  = "\033[0m";
   static constexpr auto CLR_RED    = "\033[1;31m";
   static constexpr auto CLR_ORANGE = "\033[38;5;208m";

   std::ifstream fs(file);
   if (!fs.is_open()) return;

   std::string text;
   int current = 0;
   int start = (target_line - range > 0) ? target_line - range : 1;
   int end = target_line + range;

   while (std::getline(fs, text)) {
      current++;
      if (current >= start && current <= end) {
         std::printf("├ ");
         std::printf("%s%4d%s  ", CLR_ORANGE, current, CLR_RESET);
         if (current == target_line) {
            std::printf("%s>> %s%s\n", CLR_RED, text.c_str(), CLR_RESET);
         } else {
            std::printf("   %s\n", text.c_str());
         }
      }
      if (current > end) break;
   }
}

extern "C" void cortos_port_system_error(uintptr_t auxilary1, uintptr_t auxilary2, char const* file_optional, int line_optional)
{
   std::printf("KERNEL PANIC at %s:%d\n", file_optional, line_optional);
   print_formatted_context(file_optional, line_optional);
   std::printf("└ AUX1: 0x%lX, AUX2: 0x%lX\n", auxilary1, auxilary2);
   std::terminate();
}

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
