#include <iostream>
#include <port.h>
#include <cstdlib>
#include <atomic>

static std::atomic<bool> need_reschedule{false};

extern "C" void rtk_on_tick(void)            { need_reschedule.store(true, std::memory_order_relaxed); }
extern "C" void rtk_request_reschedule(void) { need_reschedule.store(true, std::memory_order_relaxed); }

static port_context_t* alloc_port_context()
{
   size_t const size      = port_context_size();
   size_t const alignment = port_stack_align();
   size_t const allocate  = ((size + alignment - 1) / alignment) * alignment;

   return static_cast<port_context_t*>(std::aligned_alloc(alignment, allocate));
}
static void free_port_context(port_context_t* port_context_handle) { free(port_context_handle); }

static port_context_t *cur, *t1c, *t2c;

static void pick_and_switch()
{
   static bool flip = false;
   port_context_t* next = flip ? t1c : t2c;
   flip = !flip;
   if (next != cur) {
      port_switch(&cur, next);
      cur = next;
   }
}

static void thread_fn(void* arg)
{
   const char* name = static_cast<const char*>(arg);
   while (true)
   {
      // Pretend to work a bit
      for (int i = 0; i < 300000; ++i) { (void)0; }
      std::cout << "I am " << name << std::endl;
      port_yield(); // Ask scheduler to switch
   }
}

int main()
{
   port_init(1000);

   // stacks (Boost continuation backend currently ignores these, that’s fine)
   alignas(16) static unsigned char s1[16*1024];
   alignas(16) static unsigned char s2[16*1024];

   // allocate opaque contexts
   t1c = alloc_port_context();
   t2c = alloc_port_context();
   if (!t1c || !t2c) { std::puts("alloc failed"); return 1; }

   port_context_init(t1c, s1, sizeof s1, thread_fn, (void*)"T1 tick");
   port_context_init(t2c, s2, sizeof s2, thread_fn, (void*)"T2 tock");

   cur = t1c;

   // start first thread; after it returns (on yield), run a tiny scheduler loop
   port_start_first(t1c); // enters T1; on yield we’re back here

   while (true)
   {
      if (need_reschedule.exchange(false)) pick_and_switch();
      // a tiny pause avoids pegging the host CPU in this sim
      for (int i = 0; i < 10000; ++i) { (void)0; }
   }

   free_port_context(t1c);
   free_port_context(t2c);
   return 0;
}
