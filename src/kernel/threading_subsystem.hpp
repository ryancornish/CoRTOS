#ifndef CORTOS_THREADING_SUBSYSTEM_HPP
#define CORTOS_THREADING_SUBSYSTEM_HPP

#include <cortos/kernel/waitable.hpp>
#include <cortos/config/config.hpp>
#include <cortos/port/port.h>

#include "align.hpp"
#include "waitable_utilities.hpp"
#include "wait_subsystem.hpp"

#include <bitset>
#include <limits>

namespace cortos
{

void thread_launcher(void* tcb_ptr);

class ThreadTermination : public Waitable
{
   std::atomic<bool> terminated{false};

public:
   [[nodiscard]] bool has_terminated()
   {
      return terminated.load(std::memory_order_acquire);
   }

   void terminate()
   {
      bool expected = false;
      CORTOS_ASSERT(terminated.compare_exchange_strong(expected, true, std::memory_order_release));

      signal_all(false);
   }
};

struct ThreadControlBlock
{
   enum class State : uint8_t { Ready, Running, Blocked, Terminated };
   State state{State::Ready};

   // Intrusive 'linked-list' links for a ThreadReadyQueue
   ThreadControlBlock* next{nullptr};
   ThreadControlBlock* prev{nullptr};

   uint32_t id;
   uint8_t base_priority;
   uint8_t effective_priority; // Can change dynamically

   // Core pinning
   std::uint32_t pinned_core{0};
   CoreAffinity  affinity;

   std::span<std::byte> stack;
   Thread::EntryFn entry;

   WaitGroup wait_group{};
   WaitNodePool wait_nodes{this};

   // Thread-joining waitable
   ThreadTermination termination;

   // Opaque, in-place port context storage
   alignas(CORTOS_PORT_CONTEXT_ALIGN) std::array<std::byte, CORTOS_PORT_CONTEXT_SIZE> context_storage{};
   [[nodiscard]] constexpr auto*       context()       noexcept { return reinterpret_cast<cortos_port_context_t*      >(context_storage.data()); }
   [[nodiscard]] constexpr auto const* context() const noexcept { return reinterpret_cast<cortos_port_context_t const*>(context_storage.data()); }

   [[nodiscard]] constexpr bool is_enqueued() const noexcept
   {
      return next != nullptr || prev != nullptr;
   }

   [[nodiscard]] constexpr bool is_higher_priority_than(ThreadControlBlock& rhs)  const noexcept
   {
      return effective_priority < rhs.effective_priority;
   }
   [[nodiscard]] constexpr bool is_higher_priority_than(uint8_t priority_level) const noexcept
   {
      return effective_priority < priority_level;
   }

   [[nodiscard]] Waitable::Waiter create_waiter() const noexcept
   {
      return {
         .id                 = id,
         .base_priority      = base_priority,
         .effective_priority = effective_priority,
         .pinned_core        = pinned_core,
         .affinity           = affinity,
      };
   }

   ThreadControlBlock(uint32_t id, Thread::Priority priority, CoreAffinity affinity, std::span<std::byte> stack, Thread::EntryFn&& entry);

   void prepare_block(std::span<Waitable* const> waitables);

   void notify_block(std::span<Waitable* const> waitables) const;

   Waitable::Result commence_block();

   void teardown_wait_group(WaitGroup& group) noexcept;
};


/**
 * Carves a user-provided buffer region into:
 * +----------------------+ <-- buffer's end (high address)
 * +   ThreadControlBlock   + (Fixed size)
 * +----------------------+
 * + Thread-local storage + (Variable size)
 * +----------------------+
 * +     User's stack     +
 * +----------------------+ <-- buffer's base (low address)
 */
struct StackLayout
{
   ThreadControlBlock* tcb;
   std::span<std::byte> tls_region;
   std::span<std::byte> user_stack;

   explicit StackLayout(std::span<std::byte> buffer, std::size_t tls_bytes);
};

class ThreadReadyQueue
{
private:
   ThreadControlBlock* head{nullptr};
   ThreadControlBlock* tail{nullptr};

public:
   [[nodiscard]] constexpr bool empty() const noexcept
   {
      return !head;
   }

   [[nodiscard]] constexpr bool has_peer() const noexcept
   {
      return head && head != tail;
   }

   [[nodiscard]] constexpr ThreadControlBlock* front() const noexcept
   {
      return head;
   }

   // This walks the linked list so isn't 'free'
   [[nodiscard]] constexpr std::size_t size() const noexcept
   {
      std::size_t n = 0;
      for (auto* tcb = head; tcb; tcb = tcb->next) ++n;
      return n;
   }

   void push_back(ThreadControlBlock& tcb) noexcept;

   ThreadControlBlock* pop_front() noexcept;

   void remove(ThreadControlBlock& tcb) noexcept;
};

class ThreadReadyMatrix
{
private:
   static constexpr std::size_t BITMAP_BITS = std::numeric_limits<uint32_t>::digits;
   std::array<ThreadReadyQueue, config::MAX_PRIORITIES> matrix{};
   uint32_t bitmap{0};
   static_assert(config::MAX_PRIORITIES <= BITMAP_BITS, "bitmap cannot hold that many priorities!");

public:
   constexpr ThreadReadyMatrix() = default;

   [[nodiscard]] constexpr int best_priority() const noexcept
   {
      return bitmap ? std::countr_zero(bitmap) : -1;
   }

   [[nodiscard]] constexpr bool empty() const noexcept
   {
      return bitmap == 0;
   }

   [[nodiscard]] constexpr bool empty_at(uint32_t priority) const noexcept
   {
      return matrix[priority].empty();
   }

   [[nodiscard]] constexpr bool has_peer(uint32_t priority) const noexcept
   {
      return matrix[priority].has_peer();
   }

   [[nodiscard]] constexpr std::size_t size_at(uint32_t priority) const noexcept
   {
      return matrix[priority].size();
   }

   [[nodiscard]] constexpr std::bitset<BITMAP_BITS> bitmap_view() const noexcept
   {
      return {bitmap};
   }

   [[nodiscard]] constexpr ThreadControlBlock* peek_best_thread() const noexcept
   {
      if (bitmap == 0) return nullptr;
      auto const priority = std::countr_zero(bitmap);
      return matrix[priority].front();
   }

   void enqueue_thread(ThreadControlBlock& tcb) noexcept;

   ThreadControlBlock* pop_best_thread() noexcept;

   void remove_thread(ThreadControlBlock& tcb) noexcept;
};

} // namespace cortos

#endif // CORTOS_THREADING_SUBSYSTEM_HPP
