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
   //-------------- Config ---------------
   static constexpr uint32_t MAX_PRIORITIES = 32; // 0 = highest, 31 = lowest
   static constexpr uint32_t MAX_THREADS    = 64;
   static constexpr uint32_t TIME_SLICE     = 10; // In ticks

   static_assert(MAX_PRIORITIES <= std::numeric_limits<uint32_t>::digits, "Unsupported configuration");

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

   class Thread
   {
      struct TaskControlBlock* tcb;

   public:
      using Id = std::uint32_t;

      struct Entry
      {
         std::variant<std::monostate, void(*)(void*), void(*)()> fn;
         void* arg{nullptr};
         constexpr Entry() = default;
         Entry(void(*fn)()) : fn(fn) {} // Intentionally implicit
         Entry(void(*fn)(void*), void* arg) : fn(fn), arg(arg) {}
         void operator()() const { std::visit([this](auto const& func) {
            if constexpr (std::is_same_v<std::decay_t<decltype(func)>, void(*)(void*)>) func(arg);
            else if constexpr (std::is_same_v<std::decay_t<decltype(func)>, void(*)()>) func();   }, fn);
         }
      };

      struct Priority
      {
         std::uint8_t val;
         constexpr Priority(std::uint8_t v) : val(v) {} // Intentionally implicit
         operator uint8_t() const { return val; } // Intentionally implicit
      };

      Thread(Entry entry, std::span<std::byte> stack, Priority priority);
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


} // namespace rtk

#endif
