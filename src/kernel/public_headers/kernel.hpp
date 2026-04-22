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
#include <span>

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


/**
 * @brief Core affinity mask
 *
 * Bit flags indicating which cores a thread can run on.
 * Use bitwise OR to combine cores: Core0 | Core1
 */
struct CoreAffinity
{
   std::uint32_t mask;
   constexpr explicit CoreAffinity(std::uint32_t m) : mask(m) {}
   constexpr explicit operator std::uint32_t() const { return mask; }
   constexpr CoreAffinity operator|(CoreAffinity rhs) const { return CoreAffinity{mask | rhs.mask}; }
   constexpr CoreAffinity operator&(CoreAffinity rhs) const { return CoreAffinity{mask & rhs.mask}; }
   [[nodiscard]] constexpr bool allows(uint32_t core_id) const noexcept { return (mask & (1u << core_id)) != 0; }
   [[nodiscard]] constexpr static CoreAffinity from_id(std::uint32_t core_id) { return CoreAffinity{1u << core_id}; }
};
// Predefined core masks
static constexpr CoreAffinity Core0 = CoreAffinity{0x01};
static constexpr CoreAffinity Core1 = CoreAffinity{0x02};
static constexpr CoreAffinity Core2 = CoreAffinity{0x04};
static constexpr CoreAffinity Core3 = CoreAffinity{0x08};
static constexpr CoreAffinity AnyCore = CoreAffinity{0xFFFFFFFF};


/**
 * @brief Joinable CoRTOS thread handle.
 *
 * Owns a running kernel task. The task's TCB is constructed inside the user-provided
 * stack buffer, so both the @c Thread object and the stack buffer must outlive the task.
 *
 * The destructor asserts the thread is terminated (i.e. no implicit detach).
 */
class Thread
{
public:
   using Id = std::uint32_t;
   using EntryFn = Function<void(), 48, HeapPolicy::NoHeap>;

   struct Priority
   {
      std::uint8_t val;
      constexpr Priority(std::uint8_t v) : val(v) {}     // Intentionally implicit
      constexpr operator uint8_t() const { return val; } // Intentionally implicit
   };


   /**
    * @brief Create empty thread handle.
    * A registered Thread can be moved into this.
    */
   constexpr Thread() = default;
   /**
    * @brief Create and register a new thread.
    * @param entry Thread entry function.
    * @param stack User-owned stack buffer (must remain valid until termination).
    * @param priority Initial priority.
    * @param affinity Core affinity (defaults to AnyCore).
    */
   Thread(EntryFn&& entry, std::span<std::byte> stack, Priority priority, CoreAffinity affinity = AnyCore);
   ~Thread();
   Thread(Thread&&) noexcept;
   Thread& operator=(Thread&&) noexcept;
   Thread(Thread const&)            = delete;
   Thread& operator=(Thread const&) = delete;

   /**
    * @brief Get thread ID
    * @return Unique thread identifier
    */
   [[nodiscard]] Id get_id() const noexcept;

   /**
    * @brief Get thread priority
    * @return Current effective priority (base + inherited)
    */
   [[nodiscard]] Priority priority() const noexcept;

   /**
    * @brief Wait for thread to exit
    *
    * Blocks until this thread terminates.
    * Can only be called once per thread.
    */
   void join() noexcept;


   static std::size_t reserved_stack_size();

private:
   struct TaskControlBlock* tcb{nullptr};
};

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
   constexpr Spinlock() : flag(ATOMIC_FLAG_INIT) {}

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
   void unlock();

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

/**
 * @brief Base class for objects that can block threads
 *
 * Waitable is inherited by synchronization primitives (Mutex, Semaphore, etc.)
 * and time-aware objects (Timer). It provides hooks for custom behavior when
 * threads block/wake on the object.
 *
 * Threads do NOT call methods on Waitable directly. Instead, use the free
 * functions kernel::wait_for() and kernel::wait_for_any().
 *
 * Example (Timer in libcortos):
 *   class Timer : public Waitable
 *   {
 *      TimePoint wakeup_time;
 *      // TimeDriver calls wake_one() when time expires
 *   };
 *
 *   Timer timer;
 *   Mutex mutex;
 *   auto result = kernel::wait_for_any({&mutex, &timer});
 *   // Woken by whichever fired first
 */
class Waitable
{
public:
   using Predicate = Function<bool(), 64, HeapPolicy::NoHeap>;

   /**
   * @brief Result of a wait operation
   *
   * Returned from `wait_for()` and `wait_for_any()` to indicate which `Waitable`
   * triggered the wake-up and whether the thread acquired a resource.
   */
   struct Result
   {
      int  index{-1};       ///< Index of waitable that triggered (-1 if none)
      bool acquired{false}; ///< True if resource was acquired (e.g., mutex locked)
   };
   static_assert(std::is_trivially_copyable_v<Waitable::Result>, "Waitable::Result must be trivially copyable");

   /**
   * @brief Snapshot of a thread waiting on a Waitable
   *
   * Waiter is a lightweight, read-only snapshot of a thread at the moment it
   * blocks on or is removed from a Waitable. It contains no ownership or control
   * semantics and is safe to copy and store.
   */
   struct Waiter
   {
      Thread::Id       id;                  ///< Unique thread identifier
      Thread::Priority base_priority;       ///< Thread's base (static) priority
      Thread::Priority effective_priority;  ///< Thread's effective priority at snapshot time
      std::uint32_t    pinned_core;         ///< Core the thread is pinned to
      CoreAffinity     affinity;            ///< Core affinity mask

      /**
      * @brief Compare priority against another waiter
      * @return true if this waiter has higher scheduling priority than rhs
      */
      [[nodiscard]] constexpr bool higher_priority_than(Waiter const& rhs) const noexcept
      {
         return effective_priority.val < rhs.effective_priority.val;
      }
   };
   static_assert(std::is_trivially_copyable_v<Waitable::Waiter>, "Waitable::Waiter must be trivially copyable");

   virtual ~Waitable() = default;

   Waitable(Waitable const&)            = delete;
   Waitable& operator=(Waitable const&) = delete;
   Waitable(Waitable&&)            = delete;
   Waitable& operator=(Waitable&&) = delete;

   /**
    * @brief Check if any threads are waiting
    * @return true if wait queue is empty, false if threads are waiting
    */
   [[nodiscard]] bool empty() const noexcept;

   /**
    * @brief Signal one waiting thread (highest priority)
    * @param acquired True if signalled thread acquired the resource (e.g., mutex lock)
    *
    * Moves the highest-priority waiting thread to the ready queue.
    * If no threads are waiting, this is a no-op.
    *
    * The 'acquired' parameter is returned in Waitable::Result:
    * - Mutex::unlock() -> wake_one(true)  // Woken thread now owns mutex
    * - Semaphore::post() -> wake_one(false) // Woken thread is just notified
    * - Timer::expire() -> wake_one(false)   // Woken thread didn't acquire anything
    *
    * Called by the owning primitive (e.g., Mutex::unlock(), Timer expiry).
    */
   void signal_one(bool acquired = true) noexcept;

   /**
    * @brief Signal all waiting threads
    * @param acquired True if signalled threads acquired the resource
    *
    * Moves all waiting threads to the ready queue.
    * If no threads are waiting, this is a no-op.
    */
   void signal_all(bool acquired = true) noexcept;

protected:
   // Abstract Base Class
   Waitable() = default;

   /**
    * @brief Called when a thread blocks on this waitable
    * @param waiter Details of the blocking thread
    *
    * Override to implement custom behavior (e.g., priority inheritance).
    * Called before thread is added to wait queue.
    * @note no-op when not overridden
    */
   virtual void on_thread_blocked(Waiter waiter) {}

   /**
    * @brief Called when a thread is removed from wait queue
    * @param waiter Details of the thread being removed (woken or cancelled)
    *
    * Override to implement cleanup (e.g., clear inherited priority).
    * Called after thread is removed from wait queue.
    * @note no-op when not overridden
    */
   virtual void on_thread_removed(Waiter waiter) {}

   using WaiterVisitor = Function<void(Waiter const&), 64, HeapPolicy::NoHeap>;
   /**
    * @brief Visit each thread currently waiting on this Waitable
    *
    * Calls @p visitor once per waiter with a snapshot taken during traversal.
    * @warning The visitor must not block.
    *
    * Example:
    * @code
    * Priority max_priority = Priority{0};
    * for_each_waiter([&](Waiter w) {
    *    if (w.effective_priority > max_priority) {
    *       max_priority = w.effective_priority;
    *    }
    * });
    */
   void for_each_waiter(WaiterVisitor visitor) const;

private:
   friend struct TaskControlBlock;
   friend struct WaitableGroupLock;

   struct WaitNode* head{nullptr};
   struct WaitNode* tail{nullptr};

   mutable Spinlock wait_lock;

   void add(WaitNode& wait_node) noexcept;
   void remove(WaitNode& wait_node) noexcept;

   // Select best waiter but do NOT unlink it.
   // Caller must hold wait_lock.
   // FIFO among equals: scan from head, pick first with highest priority.
   WaitNode* pick_best() noexcept;
};

namespace kernel
{
   /**
    * @brief Initialise the kernel
    *
    * Must be called before any threads are created or kernel functions used.
    * Sets up scheduler data structures.
    */
   void initialise();

   /**
    * @brief Start the scheduler
    *
    * At least one thread must exist before calling start().
    */
   void start();

   void finalise();

   /**
    * @brief Get total number of CPU cores
    * @return Number of cores (1 for single-core)
    */
   [[nodiscard]] std::uint32_t core_count() noexcept;

   /**
    * @brief Get total number of currently registered threads
    *
    * Intended for diagnosis only. All threads that register add to the tally.
    * All threads that terminate substract from the tally.
    */
   [[nodiscard]] std::uint32_t active_threads() noexcept;

   /**
   * @brief Block current thread until ANY of the given waitables is signalled.
   *
   * Low-level overload taking a span of waitable pointers.
   * Prefer the templated wait_for_any(Waitables&...) overload in user code.
   * Notification semantics: signals are not persisted. If no waiter is present
   * when signal_one/all occurs, the signal is lost.
   *
   * @param waitables Non-empty list of waitables (must remain valid for the wait duration).
   * @return Result: index of the signalled waitable and whether it was acquired.
   */
   Waitable::Result wait_for_any(std::span<Waitable* const> waitables);

   /**
    * @brief Block current thread until `predicate` returns true, waking on any waitable.
    *
    * Predicate semantics:
    * - Atomically checks `predicate` and (if false) enqueues the current thread on all
    *   waitables before blocking.
    * - When woken by any waitable, re-checks `predicate`. If still false, it re-enqueues
    *   and blocks again.
    *
    * This prevents lost wakeups for stateful conditions (mutex available, count>0,
    * thread terminated), while still allowing additional wake sources (e.g. timer).
    *
    * Return value:
    * - If `predicate` is already true on entry, returns {index=-1, acquired=false}.
    * - Otherwise returns the last wake source observed before `predicate` became true
    *   (index in [0..N-1]) and the acquired flag from that wake.
    */
   Waitable::Result wait_until(Waitable::Predicate predicate, std::span<Waitable* const> waitables);

   /**
   * @brief Block current thread until ANY of the given waitables is signalled. (Preferred)
   *
   * Convenience overload that accepts references and forwards to the span overload.
   *
   * @tparam Waitables One or more waitable types.
   * @param waitables One or more waitables (must remain valid for the wait duration).
   * @return Result: index of the signalled waitable and whether it was acquired.
   */
   template<typename... Waitables>
   inline Waitable::Result wait_for_any(Waitables&... waitables)
   {
      static_assert(sizeof...(Waitables) > 0);
      return wait_for_any(std::initializer_list<Waitable* const>{ (&waitables)... });
   }

   template<typename... Waitables>
   inline Waitable::Result wait_until(Waitable::Predicate predicate, Waitables&... waitables)
   {
      static_assert(sizeof...(Waitables) > 0);
      return wait_until(std::move(predicate), std::initializer_list<Waitable* const>{ (&waitables)... });
   }

   /**
   * @brief Block current thread on a single waitable.
   *
   * Equivalent to wait_for_any(waitable).
   *
   * @param waitable Waitable to block on (must remain valid for the wait duration).
   * @return Result for the wait (index will be 0).
   */
   inline Waitable::Result wait_for(Waitable& waitable)
   {
      return wait_for_any(waitable);
   }

}  // namespace kernel


namespace this_thread
{
   /**
    * @brief Get current thread ID
    */
   [[nodiscard]] ::cortos::Thread::Id id();

   /**
    * @brief Get current thread (effective) priority
    */
   [[nodiscard]] ::cortos::Thread::Priority priority();

   /**
    * @brief Get current CPU core ID (0-based)
    */
   [[nodiscard]] std::uint32_t core_id() noexcept;

   /**
    * @brief Exit current thread
    *
    * Marks current thread as Terminated. Thread never runs again.
    * Scheduler switches to next ready thread.
    *
    * Note: If thread entry function returns, this is called automatically.
    */
   [[noreturn]] void thread_exit();

   void yield();

}  // namespace this_thread

} // namespace cortos

#endif // CORTOS_KERNEL_HPP
