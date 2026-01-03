/**
 * @file kernel.hpp
 * @brief CoRTOS Kernel API
 *
 * This is the main kernel header. It contains all kernel primitives and APIs.
 */

#ifndef CORTOS_KERNEL_HPP
#define CORTOS_KERNEL_HPP

#include <array>
#include <atomic>
#include <cstddef>

namespace cortos
{


/* ============================================================================
 * Function - Type-Erased Callable with Configurable Storage
 * ========================================================================= */

/**
 * @brief Heap allocation policy for Function
 */
enum class HeapPolicy
{
   NoHeap,      // Compile error if callable doesn't fit inline storage
   CanUseHeap,  // Use inline storage if possible, heap otherwise
   MustUseHeap  // Always allocate on heap
};

/**
 * @brief Type-erased callable with deterministic storage semantics
 *
 * Similar to std::function but with explicit control over memory allocation:
 * - Configurable inline storage size
 * - Compile-time heap policy enforcement
 * - Move-only semantics (no accidental copies)
 *
 * Unlike std::function, you have full control over when/if heap allocation occurs.
 *
 * @tparam Signature Function signature (e.g., void(), int(float))
 * @tparam InlineSize Size of inline storage buffer in bytes
 * @tparam Policy Heap allocation policy
 *
 * Example:
 *   Function<void(), 32, HeapPolicy::NoHeap> callback;
 *   callback = []() { do_work(); };  // Compiles if lambda fits in 32 bytes
 *
 *   int x = 42;
 *   callback = [x]() { use(x); };  // May not fit - compile error with NoHeap
 */
template<typename Signature, std::size_t InlineSize = 32, HeapPolicy Policy = HeapPolicy::NoHeap>
class Function;

/**
 * @brief Function specialization for function signatures
 */
template<typename Ret, typename... Args, std::size_t InlineSize, HeapPolicy Policy>
class Function<Ret(Args...), InlineSize, Policy>
{
   static constexpr bool AllowHeap = (Policy != HeapPolicy::NoHeap);
   static constexpr bool ForceHeap = (Policy == HeapPolicy::MustUseHeap);

   using InvokeFn  = Ret(*)(void*, Args&&...);
   using MoveFn    = void(*)(void*, void*);
   using DestroyFn = void(*)(void*);

   struct VTable
   {
      InvokeFn  invoke;
      MoveFn    move;
      DestroyFn destroy;
   };

   VTable const* vtable{nullptr};

   // Storage for either inline object or heap pointer
   union Storage
   {
      alignas(std::max_align_t) std::array<std::byte, InlineSize> inline_storage;
      void* heap_ptr;
   } storage{};

public:
   constexpr Function() = default;
   constexpr Function(std::nullptr_t) noexcept {}

   /**
    * @brief Construct from callable
    * @tparam F Callable type (lambda, function pointer, functor)
    */
   template<typename F>
   Function(F&& f)
   {
      emplace(std::forward<F>(f));
   }

   ~Function()
   {
      reset();
   }

   Function(Function&& other) noexcept
   {
      move_from(std::move(other));
   }

   Function& operator=(Function&& other) noexcept
   {
      if (this != &other) {
         reset();
         move_from(std::move(other));
      }
      return *this;
   }

   Function(Function const&) = delete;
   Function& operator=(Function const&) = delete;

   /**
    * @brief Replace current callable with a new one
    * @tparam F Callable type
    */
   template<typename F>
   void emplace(F&& f)
   {
      using Decayed = std::decay_t<F>;

      // Verify callable signature matches
      static_assert(std::is_invocable_r_v<Ret, Decayed&, Args...>,
                    "Callable signature does not match Function signature");

      constexpr std::size_t FuncSize      = sizeof(Decayed);
      constexpr bool FitsInline           = (FuncSize <= InlineSize);
      constexpr bool NeedsHeapForSize     = !FitsInline;
      constexpr bool UseHeap              = ForceHeap || NeedsHeapForSize;

      // Enforce heap policy
      static_assert(!NeedsHeapForSize || AllowHeap,
                    "Callable too large for inline storage. "
                    "Increase InlineSize or allow heap allocation.");

      reset(); // Destroy old callable if any

      if constexpr (UseHeap) {
         static_assert(AllowHeap, "Heap usage is disabled for this Function");
         auto* ptr = new Decayed(std::forward<F>(f));
         storage.heap_ptr = ptr;
         vtable = &VTableImpl<Decayed, true>::table;
      } else {
         void* buf = &storage.inline_storage;
         new (buf) Decayed(std::forward<F>(f));
         vtable = &VTableImpl<Decayed, false>::table;
      }
   }

   /**
    * @brief Invoke the stored callable
    *
    * Note: operator() is const because it doesn't change which callable is stored,
    * but the callable itself may mutate its internal state (e.g., captured variables).
    */
   Ret operator()(Args... args) const
   {
      // const_cast is safe: we're delegating to the stored callable
      return vtable->invoke(const_cast<Function*>(this), std::forward<Args>(args)...);
   }

   /**
    * @brief Check if Function contains a callable
    * @return true if callable is stored, false if empty
    */
   explicit operator bool() const noexcept
   {
      return vtable != nullptr;
   }

   /**
    * @brief Clear the stored callable
    */
   void reset() noexcept
   {
      if (vtable) {
         vtable->destroy(this);
         vtable = nullptr;
      }
   }

private:
   template<typename F, bool Heap>
   struct VTableImpl
   {
      static Ret invoke(void* self_void, Args&&... args)
      {
         auto* self = static_cast<Function*>(self_void);
         F* obj = get(self);
         return (*obj)(std::forward<Args>(args)...);
      }

      static void move(void* dst_void, void* src_void)
      {
         auto* dst = static_cast<Function*>(dst_void);
         auto* src = static_cast<Function*>(src_void);

         if constexpr (Heap) {
            dst->storage.heap_ptr = src->storage.heap_ptr;
            src->storage.heap_ptr = nullptr;
         } else {
            F* src_obj = get(src);
            void* dst_buf = &dst->storage.inline_storage;
            new (dst_buf) F(std::move(*src_obj));
            src_obj->~F();
         }

         dst->vtable = src->vtable;
         src->vtable = nullptr;
      }

      static void destroy(void* self_void)
      {
         auto* self = static_cast<Function*>(self_void);

         if constexpr (Heap) {
            if (self->storage.heap_ptr) {
               delete static_cast<F*>(self->storage.heap_ptr);
               self->storage.heap_ptr = nullptr;
            }
         } else {
            F* obj = get(self);
            obj->~F();
         }
      }

      static F* get(Function* self)
      {
         if constexpr (Heap) {
            return static_cast<F*>(self->storage.heap_ptr);
         } else {
            return std::launder(reinterpret_cast<F*>(&self->storage.inline_storage));
         }
      }

      static const VTable table;
   };

   template<typename F, bool Heap>
   friend struct Function::VTableImpl;

   void move_from(Function&& other) noexcept
   {
      if (!other.vtable) {
         vtable = nullptr;
         return;
      }
      other.vtable->move(this, &other);
   }
};

// Out-of-line VTable definition
template<typename Ret, typename... Args, std::size_t InlineSize, HeapPolicy Policy>
template<typename F, bool Heap>
const typename Function<Ret(Args...), InlineSize, Policy>::VTable
Function<Ret(Args...), InlineSize, Policy>::VTableImpl<F, Heap>::table{
   .invoke  = &VTableImpl::invoke,
   .move    = &VTableImpl::move,
   .destroy = &VTableImpl::destroy
};


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
