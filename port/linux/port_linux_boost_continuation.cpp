#include <port.h>
#include <port_traits.h>
#include <boost/context/continuation.hpp>
#include <boost/context/preallocated.hpp>
#include <boost/context/stack_context.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>  // allocator for callcc
#include <atomic>
#include <csignal>
#include <sys/time.h>
#include <cstdint>
#include <cstring>

extern "C" {
#include <port.h>
}

static std::atomic<uint32_t> g_tick{0};
uint32_t port_tick_now() { return g_tick.load(std::memory_order_relaxed); }

void port_isr_enter() {}
void port_isr_exit(int) {}
void port_preempt_disable() {}
void port_preempt_enable() {}
void port_irq_disable() {}
void port_irq_enable() {}

struct port_context
{
   boost::context::continuation cont;  // Thread continuation (parked when not running)
   boost::context::continuation sched; // Scheduler continuation to yield back to
   void*        stack_top;
   size_t       stack_size;
   port_entry_t entry;
   void*        arg;
   bool         started;
};
static_assert(RTK_PORT_CONTEXT_SIZE == sizeof(port_context_t));
static_assert(RTK_PORT_CONTEXT_ALIGN == alignof(port_context_t));
static_assert(RTK_STACK_ALIGN == 16);

// "Current" context for this OS thread (our sim is single core, single OS thread)
static thread_local port_context* tls_current = nullptr;

struct preallocated_stack_noop_stub
{
   using traits_type = boost::context::stack_traits;
   boost::context::stack_context allocate(std::size_t) { std::abort(); }
   // no-op: memory owned by caller (scheduler)
   void deallocate(boost::context::stack_context&) noexcept {}
};

// Instead of initing on port_context_init, lazily init on start and switch
// Until I find a better way, this works for now
static void boost_context_lazy_init(port_context_t* context)
{
   if (context->started) return;
   context->started = true;

   // Build a Boost stack_context pointing to the caller-provided memory.
   // Boost expects .sp to be the *top* of the stack for downward-growing stacks.
   boost::context::stack_context stack_context = {
      .size = context->stack_size,
      .sp   = context->stack_top,
   };

   // Preallocated descriptor and allocator
   boost::context::preallocated prealloc(context->stack_top, context->stack_size, stack_context);
   preallocated_stack_noop_stub salloc;
   context->cont = boost::context::callcc(std::allocator_arg, prealloc, salloc,
      [context](boost::context::continuation&& sched_in) mutable -> boost::context::continuation
      {
          // Park here on first entry
          // sched_in is the scheduler continuation
         context->sched = std::move(sched_in);

         while (true)
         {
            tls_current = context;                 // we're the running thread
            context->entry(context->arg);          // if it returns, thread is "done"
            // Hand control back to scheduler so it can clean up/decide next.
            tls_current = nullptr;
            (void)context->sched.resume();  // yield back to scheduler
         }
      }
   );
}

void port_context_init(port_context_t* context, void* stack_base, size_t stack_size, port_entry_t entry, void* arg)
{
   ::new (context) port_context{
      .cont  = {},
      .sched = {},
      .stack_top  = static_cast<uint8_t*>(stack_base) + stack_size,
      .stack_size = stack_size,
      .entry      = entry,
      .arg        = arg,
      .started    = false,
   };
}

// Honestly this stuff is a bit magic. I'll need to come back and read the boost context docs at some point
void port_context_destroy(port_context_t* ctx)
{
   // If still valid, request it to unwind to completion
   if (ctx->cont) {
      ctx->cont = std::move(ctx->cont).resume_with([](boost::context::continuation&&)
      {
         return boost::context::continuation{}; // Return an empty continuation to signal completion
      });
   }
   ctx->~port_context();
}

static thread_local void* thread_pointer = nullptr;

void port_set_thread_pointer(void* tp) { thread_pointer = tp; }
void* port_get_thread_pointer(void)    { return thread_pointer; }

void port_switch(port_context_t* /*from*/, port_context_t* to)
{
   tls_current = to;
   boost_context_lazy_init(to); // TODO Is there a better way?
   // Resume target. When the thread yields, control returns here.
   to->cont = to->cont.resume();
   // After returning, no thread is "current" (we're back in scheduler).
   tls_current = nullptr;
}

void port_start_first(port_context_t* first)
{
   tls_current = first;
   boost_context_lazy_init(first); // TODO Is there a better way?
   first->started = true;
   // Enter first thread. In a real kernel, scheduler loop keeps running after returns.
   first->cont = first->cont.resume();
   tls_current = nullptr;
   // Normal MCU's do not return from port_start_first... but Boost does
   //__builtin_unreachable();
}

void port_yield()
{
   // In continuation model, the thread must explicitly resume the scheduler.
   if (tls_current) {
      // Clear the current thread marker before bouncing back to the scheduler
      auto* current = tls_current;
      tls_current = nullptr;
      (void)std::move(current->sched).resume();
   } else {
      // If somehow called outside a thread, just request a reschedule.
      rtk_request_reschedule();
   }
}

void port_idle()
{
   // Sleep 1 ms
   struct timespec req{.tv_nsec = 1'000'000};
   nanosleep(&req, nullptr);
   port_yield(); // Under linux sim, we must explicitly resume the scheduler.
}

// Tick source: SIGALRM -> bump tick & call kernel hook.
static void tick_handler(int)
{
   g_tick.fetch_add(1, std::memory_order_relaxed);
   rtk_on_tick();
}

// Sets up a SIGALRM timer firing every 'tick_hz' ms.
// Installs signal handler 'tick_handler()' to increment global tick counter and call rtk_on_tick().
// Essentially simulating a 'system tick interrupt' on linux.
void port_init(uint32_t tick_hz)
{
   if (!tick_hz) tick_hz = 1000;

   static std::array<uint8_t, 8 * 1024> altstack;
   stack_t signal_stack_object = {
      .ss_sp = altstack.data(),
      .ss_flags = 0,
      .ss_size = altstack.size(),
   };
   sigaltstack(&signal_stack_object, nullptr);

   struct sigaction sig_action_object{};
   sig_action_object.sa_handler = tick_handler;
   sigemptyset(&sig_action_object.sa_mask);
   sig_action_object.sa_flags = SA_ONSTACK | SA_RESTART;
   sigaction(SIGALRM, &sig_action_object, nullptr);

   itimerval itimer{};
   int const  usec = 1'000'000 / tick_hz;
   itimer.it_interval.tv_sec  = usec / 1'000'000;
   itimer.it_interval.tv_usec = usec % 1'000'000;
   itimer.it_value            = itimer.it_interval;
   setitimer(ITIMER_REAL, &itimer, nullptr);
}
