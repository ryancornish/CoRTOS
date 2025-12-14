/**
 * port_linux_boost_context.cpp
 */
#include "port.h"
#include "port_traits.h"

#include <boost/context/fiber.hpp>
#include <boost/context/preallocated.hpp>
#include <boost/context/stack_context.hpp>

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <sys/time.h>

#define DEBUG_PRINT_ENABLE 1
#include "DEBUG_PRINT.hpp"

static constinit std::atomic<uint32_t> global_tick{0};
uint32_t port_tick_now() { return global_tick.load(std::memory_order_relaxed); }

// Unused/unimplemented port stubs
void port_isr_enter() {}
void port_isr_exit(int) {}
void port_preempt_disable() {}
void port_preempt_enable() {}
void port_irq_disable() {}
void port_irq_enable() {}

struct port_context
{
   boost::context::fiber thread; // thread fiber (owned by scheduler when idle)
   boost::context::fiber sched;  // scheduler fiber (owned by thread when running)
   void*        stack_top;
   std::size_t  stack_size;
   port_entry_t entry;
   void*        arg;
};
static_assert(CORTOS_PORT_CONTEXT_SIZE  == sizeof(port_context_t),  "Adjust port_traits.h definition to match");
static_assert(CORTOS_PORT_CONTEXT_ALIGN == alignof(port_context_t), "Adjust port_traits.h definition to match");
static_assert((CORTOS_STACK_ALIGN & (CORTOS_STACK_ALIGN - 1)) == 0,    "CORTOS_STACK_ALIGN must be a power of two");

// thread-local "am I inside a thread?"
static thread_local port_context* tls_current = nullptr;

// No-op stack allocator for preallocated memory
struct preallocated_stack_noop
{
  using traits_type = boost::context::stack_traits;
  boost::context::stack_context allocate(std::size_t) { std::abort(); }
  void deallocate(boost::context::stack_context&) noexcept {}
};

// Initialize an opaque port_context_t using caller-owned stack memory
// 'stack_base'/'stack_size' must obey CORTOS_STACK_ALIGN constraints
void port_context_init(port_context_t* context,
                       void* stack_base,
                       std::size_t stack_size,
                       port_entry_t entry,
                       void* arg)
{
   // Construct/emplace port_context_t object within user-provided stack
   ::new (context) port_context{
      .thread     = {},
      .sched      = {},
      .stack_top  = static_cast<std::uint8_t*>(stack_base) + stack_size,
      .stack_size = stack_size,
      .entry      = entry,
      .arg        = arg,
   };

   // Build a fiber bound to the user-provided stack.
   boost::context::stack_context boost_stack_context = {
      .size = context->stack_size,
      .sp   = context->stack_top,
   };
   boost::context::preallocated boost_prealloc(boost_stack_context.sp, boost_stack_context.size, boost_stack_context);
   preallocated_stack_noop stack_allocator;

   context->thread = boost::context::fiber(std::allocator_arg, boost_prealloc, stack_allocator,
      [context](boost::context::fiber&& sched_in) mutable -> boost::context::fiber
      {
         // First entry, save the scheduler fiber handle
         context->sched = std::move(sched_in);

         while (true) {
            tls_current = context;
            context->entry(context->arg); // Enter user code
            tls_current = nullptr;

            // Park back on scheduler until resumed again
            context->sched = std::move(context->sched).resume();
            // When resumed, we loop and re-enter user code
         }
      });
}

void port_context_destroy(port_context_t* context)
{
   // If the thread fiber still exists, try to unwind cooperatively
   if (context->thread) {
      // Ask the fiber to finish by giving it a chance to run a tiny trampoline
      context->thread = std::move(context->thread).resume_with(
         [](boost::context::fiber&& /*fb*/){ return boost::context::fiber{}; } // Return an empty fiber -> done
      );
   }
   context->~port_context();
}

static thread_local void* global_thread_pointer = nullptr;
void  port_set_thread_pointer(void* tp) { global_thread_pointer = tp; }
void* port_get_thread_pointer(void)     { return global_thread_pointer; }

// Switch into 'to' (thread). Returns when the thread yields
void port_switch(port_context_t* /*from*/, port_context_t* to)
{
   tls_current = to;
   // Enter/resume the thread fiber. Returns when thread yields back
   to->thread = std::move(to->thread).resume();
   tls_current = nullptr;
}

// Start the very first thread
void port_start_first(port_context_t* first)
{
   LOG_PORT("port_start_first()");

   tls_current = first;
   first->thread = std::move(first->thread).resume(); // Run until first yield
   tls_current = nullptr;
}

// Thread calls this to yield to scheduler
void port_yield()
{
   if (!tls_current) {
      // Special case when there is no current just reschedule!
      cortos_request_reschedule();
      return;
   }

   auto* current = tls_current;
   tls_current = nullptr;
   // Yield to the stored scheduler fiber. The returned fiber
   // is the updated scheduler handle for this thread
   current->sched = std::move(current->sched).resume();
}

void port_idle()
{
   LOG_PORT("port_idle()");

   // Sleep 1 ms (to simulate power saving) then yield cooperatively
   struct timespec req{.tv_sec = 0, .tv_nsec = 1'000'000};
   nanosleep(&req, nullptr);
   port_yield();
}

static uint32_t tick_hz;
void port_init(uint32_t tick_hz)
{
   ::tick_hz = tick_hz;
}

namespace cortos::sim
{
   void advance_tick(unsigned ticks)
   {
      global_tick.fetch_add(ticks, std::memory_order_relaxed);
      cortos_on_tick();
   }

   uint32_t get_tick_hz()
   {
      return tick_hz;
   }
}