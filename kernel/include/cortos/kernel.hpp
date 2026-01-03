/**
 * @file kernel.hpp
 * @brief CoRTOS Kernel API
 *
 * This is the main kernel header. It contains all kernel primitives and APIs.
 */

#ifndef CORTOS_KERNEL_HPP
#define CORTOS_KERNEL_HPP

#include <atomic>

namespace cortos
{

/* ============================================================================
 * Spinlock
 * ========================================================================= */

/**
 * @brief Simple spinlock for short critical sections
 *
 * Spinlocks busy-wait until acquired, so should only be held for very short
 * durations (microseconds). For longer critical sections, use a Mutex.
 *
 * Usage:
 *   Spinlock lock;
 *   lock.lock();
 *   // ... critical section ...
 *   lock.unlock();
 *
 * Or with RAII:
 *   Spinlock lock;
 *   {
 *       SpinlockGuard guard(lock);
 *       // ... critical section ...
 *   } // Automatically unlocked
 */
class Spinlock
{
public:
   Spinlock() : flag(ATOMIC_FLAG_INIT)
   {
   }

   ~Spinlock() = default;

   Spinlock(Spinlock const&)            = delete;
   Spinlock& operator=(Spinlock const&) = delete;
   Spinlock(Spinlock&&)                 = delete;
   Spinlock& operator=(Spinlock&&)      = delete;

   /**
    * @brief Acquire the spinlock (busy-wait)
    *
    * Blocks until the lock is acquired.
    * Uses CPU hints to reduce power consumption while spinning.
    */
   void lock();

   /**
    * @brief Release the spinlock
    */
   void unlock()
   {
      flag.clear(std::memory_order_release);
   }

   /**
    * @brief Try to acquire the spinlock without blocking
    * @return true if acquired, false if already locked
    */
   bool try_lock()
   {
      return !flag.test_and_set(std::memory_order_acquire);
   }

   /**
    * @brief Check if the spinlock is currently locked
    * @return true if locked, false if unlocked
    *
    * Note: This is racy and should only be used for debugging/assertions.
    */
   [[nodiscard]] bool is_locked() const
   {
      return flag.test(std::memory_order_relaxed);
   }

private:
   std::atomic_flag flag;
};

/**
 * @brief RAII guard for spinlocks
 *
 * Automatically acquires lock on construction and releases on destruction.
 */
class SpinlockGuard
{
public:
   explicit SpinlockGuard(Spinlock& lock) : lock(lock)
   {
      lock.lock();
   }

   ~SpinlockGuard()
   {
      lock.unlock();
   }

   SpinlockGuard(SpinlockGuard const&)            = delete;
   SpinlockGuard& operator=(SpinlockGuard const&) = delete;
   SpinlockGuard(SpinlockGuard&&)                 = delete;
   SpinlockGuard& operator=(SpinlockGuard&&)      = delete;

private:
   Spinlock& lock;
};

} // namespace cortos

#endif // CORTOS_KERNEL_HPP
