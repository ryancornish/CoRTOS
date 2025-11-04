/**
 * Cornish Real-Time Kernel Application Programming Interface
 *
 *
 *
 *
*/
#ifndef _CORNISH_RTK_HPP_
#define _CORNISH_RTK_HPP_

#include <cstdint>
#include <port.h>

namespace rtk
{
   //-------------- Config ---------------
   static constexpr uint32_t MAX_PRIORITIES = 32; // 0 = highest, 32 = lowest
   static constexpr uint32_t TIME_SLICE     = 10; // In ticks


   struct Scheduler
   {
      static void init(uint32_t tick_hz);
      static void start();
      static void yield();
      static uint32_t tick_now();
      static void sleep_for(uint32_t ticks);  // cooperative sleep (sim)

      static void preempt_disable();
      static void preempt_enable();

      struct Lock
      {
         Lock()  { preempt_disable(); }
         ~Lock() { preempt_enable(); }
      };
   };

   class Thread
   {
      struct TaskControlBlock* tcb;

   public:
      using EntryFunction = void(*)(void*);
      Thread(EntryFunction fn, void* arg, void* stack_base, std::size_t stack_size, uint8_t priority);
      ~Thread();
   };

   class Mutex;

   class Semaphore;

   class ConditionVar;



} // namespace rtk

#endif
