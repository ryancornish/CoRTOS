#ifndef CORTOS_THREAD_HPP
#define CORTOS_THREAD_HPP

#include <cortos/kernel/function.hpp>

#include <cstdint>
#include <span>

namespace cortos
{

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
inline constexpr CoreAffinity Core0 = CoreAffinity{0x01};
inline constexpr CoreAffinity Core1 = CoreAffinity{0x02};
inline constexpr CoreAffinity Core2 = CoreAffinity{0x04};
inline constexpr CoreAffinity Core3 = CoreAffinity{0x08};
inline constexpr CoreAffinity AnyCore = CoreAffinity{0xFFFFFFFF};


/**
 * @brief Joinable CoRTOS thread handle.
 *
 * Owns a running kernel thread. The thread's TCB is constructed inside the user-provided
 * stack buffer, so both the @c Thread object and the stack buffer must outlive the thread.
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
   struct ThreadControlBlock* tcb{nullptr};
};

namespace this_thread
{

/**
   * @brief Get current thread ID
   */
[[nodiscard]] Thread::Id id();

/**
   * @brief Get current thread (effective) priority
   */
[[nodiscard]] Thread::Priority priority();

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

#endif // CORTOS_THREAD_HPP
