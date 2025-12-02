/**
 * Cornish Real-Time Kernel Application Programming Interface
 *
 *
 *
 *
*/
#ifndef _CORNISH_RTK_HPP_
#define _CORNISH_RTK_HPP_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <variant>

namespace rtk
{
   // User Config
   struct Config
   {
      static constexpr uint32_t MAX_PRIORITIES = 32; // 0 = highest, 31 = lowest
      static constexpr uint32_t MAX_THREADS    = 64;
      static constexpr uint32_t TIME_SLICE     = 10; // In ticks

      enum class JobHeapPolicy
      {
         NoHeap,      // Compile-time assert if the callable cannot fit within the inline storage.
         CanUseHeap,  // Callable will be placed in the inline storage IF it fits, else will use the heap.
         MustUseHeap, // Callable will always be placed on the heap, even if it fits.
      };
      static constexpr JobHeapPolicy JOB_HEAP_POLICY         = JobHeapPolicy::CanUseHeap;
      static constexpr   std::size_t JOB_INLINE_STORAGE_SIZE = 16;

      static_assert(MAX_PRIORITIES <= std::numeric_limits<uint32_t>::digits, "Unsupported configuration");
   };


   class Tick
   {
      uint32_t t{0};

      static constexpr bool after_eq_u32(uint32_t a, uint32_t b) noexcept
      {
         constexpr uint32_t HALF = 1u << 31;
         return uint32_t(a - b) < HALF; // a >= b (wrap-safe)
      }

   public:
      using Delta = uint32_t;

      constexpr Tick() noexcept = default;
      constexpr explicit Tick(uint32_t value) noexcept : t(value) {}

      [[nodiscard]] constexpr uint32_t value() const noexcept { return t; }

      // Wrap-safe "now >= deadline"
      [[nodiscard]] constexpr bool has_reached(uint32_t deadline) const noexcept
      {
         return after_eq_u32(t, deadline);
      }
      [[nodiscard]] constexpr bool has_reached(Tick deadline) const noexcept
      {
         return after_eq_u32(t, deadline.t);
      }
      // Wrap-safe "now < deadline"
      [[nodiscard]] constexpr bool is_before(uint32_t deadline) const noexcept
      {
         return !after_eq_u32(t, deadline);
      }
      [[nodiscard]] constexpr bool is_before(Tick deadline) const noexcept
      {
         return !after_eq_u32(t, deadline.t);
      }

      // ---- Arithmetic ----
      // Future deadline wraps naturally
      friend constexpr Tick operator+(Tick tick, Delta delta) noexcept
      {
         return Tick(tick.t + delta);
      }
      friend constexpr Tick operator+(Delta delta, Tick tick) noexcept
      {
         return tick + delta;
      }
      Tick& operator+=(Delta delta) noexcept
      {
         t += delta; return *this;
      }

      // Elapsed ticks between two instants (mod 2^32)
      friend constexpr Delta operator-(Tick lhs, Tick rhs) noexcept
      {
         return lhs.t - rhs.t;
      }

      // ---- Comparators vs uint32_t/Tick deadlines (wrap-safe '>=' and '<') ----
      friend constexpr bool operator>=(Tick now, uint32_t deadline) noexcept
      {
         return now.has_reached(deadline);
      }
      friend constexpr bool operator<(Tick now, uint32_t deadline) noexcept
      {
         return now.is_before(deadline);
      }
      friend constexpr bool operator>=(Tick now, Tick deadline) noexcept
      {
         return now.has_reached(deadline.t);
      }
      friend constexpr bool operator<(Tick now, Tick deadline) noexcept
      {
         return now.is_before(deadline.t);
      }
   };
   static_assert(sizeof(Tick) == sizeof(uint32_t), "It is important that Tick be as cheap as uint32_t");
   static_assert(std::is_trivially_copyable_v<Tick>);

   struct Scheduler
   {
      static void init(uint32_t tick_hz);
      static void start();
      static void yield();
      static class Tick tick_now();
      static void sleep_for(uint32_t ticks);  // cooperative sleep (sim)

      static void preempt_disable();
      static void preempt_enable();

      struct Lock
      {
         Lock()  { preempt_disable(); }
         ~Lock() { preempt_enable(); }
      };
   };

   // Forward declare this because Threads like to use Jobs to launch!
   template<std::size_t InlineStorageSize, Config::JobHeapPolicy HeapPolicy> class JobModel;

   class Thread
   {
      struct TaskControlBlock* tcb;

   public:
      using Id = std::uint32_t;
      using Entry = JobModel<16, Config::JobHeapPolicy::NoHeap>;

      struct Priority
      {
         std::uint8_t val;
         constexpr Priority(std::uint8_t v) : val(v) {}     // Intentionally implicit
         constexpr operator uint8_t() const { return val; } // Intentionally implicit
      };

      Thread(Entry&& entry, std::span<std::byte> stack, Priority priority);
      ~Thread();

      [[nodiscard]] Id get_id() const noexcept;
      void join();


      static std::size_t reserved_stack_size();
   };

   // Helper template that can hide private-implementation details
   // of the public API declarations. To avoid UB, T must be trivially constructable,
   // and of standard layout. This is statically asserted within the implementation TU.
   // Essentially a compile-time, constexpr/constinit - conserving, pImpl structure.
   template <typename T, std::size_t size, std::size_t align>
   struct alignas(align) OpaqueImpl
   {
      std::byte opaque[size]{};
      constexpr OpaqueImpl() = default;
      constexpr T*       get()       noexcept { return reinterpret_cast<T*>(opaque); }
      constexpr T const* get() const noexcept { return reinterpret_cast<T const*>(opaque); }
      constexpr T&       operator* ()       noexcept { return *get(); }
      constexpr T const& operator* () const noexcept { return *get(); }
      constexpr T*       operator->()       noexcept { return  get(); }
      constexpr T const* operator->() const noexcept { return  get(); }
   };

   class Mutex
   {
   public:
      using ImplStorage = OpaqueImpl<struct MutexImpl, 40, 8>;
      constexpr Mutex() = default;
      ~Mutex()          = default;
      constexpr Mutex(Mutex&&)            = default;
      constexpr Mutex& operator=(Mutex&&) = default;
      Mutex(Mutex const&)            = delete;
      Mutex& operator=(Mutex const&) = delete;

      [[nodiscard]] bool is_locked() const noexcept;
      void lock();
      [[nodiscard]] bool try_lock();
      [[nodiscard]] bool try_lock_for(Tick::Delta timeout);
      [[nodiscard]] bool try_lock_until(Tick deadline);
      void unlock();

   private:
      friend class ConditionVar;
      ImplStorage self;
   };

   class Semaphore
   {
   public:
      using ImplStorage = OpaqueImpl<struct SemaphoreImpl, 16, 8>;
      explicit constexpr Semaphore(unsigned initial_count) noexcept : counter(initial_count) {};
      ~Semaphore() = default;
      constexpr Semaphore(Semaphore&&)            = default;
      constexpr Semaphore& operator=(Semaphore&&) = default;
      Semaphore(Semaphore const&)            = delete;
      Semaphore& operator=(Semaphore const&) = delete;

      [[nodiscard]] unsigned peek_count() const noexcept { return counter; };
      void acquire() noexcept;
      [[nodiscard]] bool try_acquire() noexcept;
      [[nodiscard]] bool try_acquire_for(Tick::Delta timeout) noexcept;
      [[nodiscard]] bool try_acquire_until(Tick deadline) noexcept;
      void release(unsigned n = 1) noexcept;

   private:
      unsigned counter;
      ImplStorage self;
   };

   class ConditionVar
   {
   public:
      using ImplStorage = OpaqueImpl<struct ConditionVarImpl, 16, 8>;
      constexpr ConditionVar() = default;
      ~ConditionVar()          = default;
      constexpr ConditionVar(ConditionVar&&)            = default;
      constexpr ConditionVar& operator=(ConditionVar&&) = default;
      ConditionVar(ConditionVar const&)            = delete;
      ConditionVar& operator=(ConditionVar const&) = delete;

      // Wait until notified; always returns with 'lock' held
      void wait(Mutex& lock) noexcept;

      // Timed waits: return true if notified; false on timeout.
      // In all cases, return with 'lock' held.
      [[nodiscard]] bool wait_for(Mutex& lock, Tick::Delta timeout) noexcept;
      [[nodiscard]] bool wait_until(Mutex& lock, Tick deadline) noexcept;

      void notify_one() noexcept;
      void notify_all() noexcept;

   private:
      ImplStorage self;
   };

   // Beware the template spaghetti!

   // An instantiation of a JobModel is similar to a 'std::function<void()>'
   // Instantiate a JobModel with:
   // - InlineStorageSize (defines the internal buffer size for capturing data accompanied by the callable).
   // - HeapPolicy (defines whether constructing a Job can/can't or will use the heap).
   template<std::size_t InlineStorageSize, Config::JobHeapPolicy HeapPolicy>
   class JobModel
   {
      static constexpr bool AllowHeap = (HeapPolicy != Config::JobHeapPolicy::NoHeap);
      static constexpr bool ForceHeap = (HeapPolicy == Config::JobHeapPolicy::MustUseHeap);
      using InvokeFn  = void(*)(void*);
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
         alignas(std::max_align_t) std::array<std::byte, InlineStorageSize> inline_storage;
         void* heap_ptr;
      } storage{};

   public:
      JobModel() = default;
      JobModel(std::nullptr_t) noexcept {}
      // Generic constructor from callable
      template<typename F> JobModel(F&& f) { emplace(std::forward<F>(f)); }
      ~JobModel() { reset(); }
      JobModel(JobModel&& other) noexcept { move_from(std::move(other)); }
      JobModel& operator=(JobModel&& other) noexcept
      {
         if (this != &other) {
            reset();
            move_from(std::move(other));
         }
         return *this;
      }
      JobModel(const JobModel&) = delete;
      JobModel& operator=(const JobModel&) = delete;

      // Replace current callable
      template<typename F>
      void emplace(F&& f)
      {
         using Decayed = std::decay_t<F>;
         static_assert(std::is_invocable_r_v<void, Decayed&>, "Job callables must be invocable as void()");
         constexpr std::size_t FuncSize  = sizeof(Decayed);
         constexpr bool FitsInline       = (FuncSize <= InlineStorageSize);
         constexpr bool NeedsHeapForSize = !FitsInline;
         constexpr bool UseHeap          = ForceHeap || NeedsHeapForSize;
         static_assert(!NeedsHeapForSize || AllowHeap, "Callable too big for this JobModel. Increase InlineStorageSize or allow heap.");

         reset(); // Destroy old callable if any
         if constexpr (UseHeap) {
            static_assert(AllowHeap, "Heap usage is disabled for this JobModel.");
            auto* ptr = new Decayed(std::forward<F>(f));
            storage.heap_ptr = ptr;
            vtable = &VTableImpl<Decayed, true>::table;
         } else {
            void* buf = &storage.inline_storage;
            new (buf) Decayed(std::forward<F>(f));
            vtable = &VTableImpl<Decayed, false>::table;
         }
      }

      void operator()() const noexcept
      {
         // const_cast is fine: we delegate constness enforcement
         if (vtable) vtable->invoke(const_cast<JobModel*>(this));
      }

      explicit operator bool() const noexcept { return vtable != nullptr; }

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
         static void invoke(void* self_void)
         {
            auto* self = static_cast<JobModel*>(self_void);
            F* obj = get(self);
            (*obj)();
         }

         static void move(void* dst_void, void* src_void)
         {
            auto* dst = static_cast<JobModel*>(dst_void);
            auto* src = static_cast<JobModel*>(src_void);
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
            auto* self = static_cast<JobModel*>(self_void);
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

         static F* get(JobModel* self)
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
      friend struct JobModel::VTableImpl;

      void move_from(JobModel&& other) noexcept
      {
         if (!other.vtable) {
            vtable = nullptr;
            return;
         }
         other.vtable->move(this, &other);
      }
   };

   // Job's VTable definition (out-of-line)
   template<std::size_t InlineStorageSize, Config::JobHeapPolicy HeapPolicy>
   template<typename F, bool Heap>
   const typename JobModel<InlineStorageSize, HeapPolicy>::VTable
   JobModel<InlineStorageSize, HeapPolicy>::VTableImpl<F, Heap>::table{
      .invoke  = &VTableImpl::invoke,
      .move    = &VTableImpl::move,
      .destroy = &VTableImpl::destroy
   };

   // User configured Job model instantiated:
   using Job = JobModel<Config::JOB_INLINE_STORAGE_SIZE, Config::JOB_HEAP_POLICY>;

} // namespace rtk

#endif
