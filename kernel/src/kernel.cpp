// Add doxygen comment here

#include "cortos/kernel.hpp"
#include "cortos/port.h"

#include <cassert>
namespace cortos
{

static constexpr std::uintptr_t align_down(std::uintptr_t v, std::size_t a) { return v & ~(static_cast<std::uintptr_t>(a) - 1); }
static constexpr std::uintptr_t   align_up(std::uintptr_t v, std::size_t a) { return (v + (a - 1)) & ~(static_cast<std::uintptr_t>(a) - 1); }

struct WaitNode
{
   struct TaskControlBlock* tcb{nullptr};
   Waitable* owner{nullptr};
   uint8_t slot{0};
   // Intrusive 'linked-list' links for a WaitQueue
   WaitNode* next{nullptr};
   WaitNode* prev{nullptr};
   struct WaitGroup* wait_group{nullptr};
};

static void thread_launcher(void* vtcb);

struct TaskControlBlock
{
   enum class State : uint8_t { Ready, Running, Blocked, Terminated };
   State state{State::Ready};

   // Intrusive 'linked-list' links for a TaskReadyQueue
   TaskControlBlock* next{nullptr};
   TaskControlBlock* prev{nullptr};
   // Intrusive link for the SleepMinHeap

   uint32_t id;
   uint8_t base_priority;
   uint8_t effective_priority; // Can change dynamically

   // Core pinning (future, but you already have affinity)
   std::uint32_t pinned_core{0};
   CoreAffinity  affinity;

   std::span<std::byte> stack;
   Thread::EntryFn entry;

   std::array<WaitNode, config::MAX_WAIT_NODES> wait_nodes{};

   // Linked-list of threads wanting to join with me
   TaskControlBlock* join_waiters_head{nullptr};

   // Opaque, in-place port context storage
   alignas(CORTOS_PORT_CONTEXT_ALIGN) std::array<std::byte, CORTOS_PORT_CONTEXT_SIZE> context_storage{};
   [[nodiscard]] constexpr auto*       context()       noexcept { return reinterpret_cast<cortos_port_context_t*      >(context_storage.data()); }
   [[nodiscard]] constexpr auto const* context() const noexcept { return reinterpret_cast<cortos_port_context_t const*>(context_storage.data()); }

   TaskControlBlock(uint32_t id, Thread::Priority priority, CoreAffinity affinity, std::span<std::byte> stack, Thread::EntryFn&& entry) :
      id(id), base_priority(priority), effective_priority(priority), affinity(affinity), stack(stack), entry(std::move(entry))
   {
      for (int i = 0; auto& wait_node : wait_nodes) {
         wait_node.tcb = this;
         wait_node.slot = i++;
      }
      cortos_port_context_init(context(), stack.data(), stack.size(), thread_launcher, this);
   }
};

static void thread_launcher(void* vtcb)
{
   auto* tcb = static_cast<TaskControlBlock*>(vtcb);
   // For now point TLS to the TCB, but in future, TLS sits just after TCB
   cortos_port_set_tls_pointer(tcb);
   tcb->entry();

   // Signal joiners

   cortos_port_thread_exit();
}

// Carves a user-provided buffer region into:
// +----------------------+ <-- buffer's end (high address)
// +   TaskControlBlock   + (Fixed size)
// +----------------------+
// + Thread-local storage + (Variable size)
// +----------------------+
// +     User's stack     +
// +----------------------+ <-- buffer's base (low address)
struct StackLayout
{
   TaskControlBlock* tcb;
   std::span<std::byte> tls_region;
   std::span<std::byte> user_stack;

   explicit StackLayout(std::span<std::byte> const buffer, std::size_t const tls_bytes)
   {
      auto const base = reinterpret_cast<std::uintptr_t>(buffer.data());
      auto const end  = base + buffer.size();

      // TCB at very top, aligned down
      auto const tcb_start = align_down(end - sizeof(TaskControlBlock), alignof(TaskControlBlock));
      tcb = reinterpret_cast<TaskControlBlock*>(tcb_start);

      // TLS just below TCB
      auto const tls_size = align_up(tls_bytes, alignof(std::max_align_t));
      auto const tls_top  = tcb_start;
      auto const tls_base = align_down(tls_top - tls_size, alignof(std::max_align_t));

      assert(tls_base >= base && "Buffer too small for TLS+TCB");

      auto const tls_offset = static_cast<std::size_t>(tls_base - base);
      auto const tls_length = static_cast<std::size_t>(tls_top  - tls_base);
      tls_region = buffer.subspan(tls_offset, tls_length); // zero-length span if tls_bytes == 0

      // User stack: everything below TLS
      auto const stack_len = static_cast<std::size_t>(tls_base - base);
      assert(stack_len > 64 && "Buffer too small after carving TCB/TLS");

      user_stack = buffer.subspan(0, stack_len);
   }
};

static std::atomic<uint32_t> next_thread_id{1};
Thread::Thread(EntryFn&& entry, std::span<std::byte> stack, Priority priority, CoreAffinity affinity)
{
   auto id = next_thread_id.fetch_add(1, std::memory_order_relaxed);

   StackLayout slayout(stack, 0);
   tcb = ::new (slayout.tcb) TaskControlBlock(id, priority, affinity, slayout.user_stack, std::move(entry));

   //set_task_ready(tcb);
}


void Spinlock::lock()
{
   while (flag.test_and_set(std::memory_order_acquire)) {
      // Busy-wait with CPU yield hint
      cortos_port_cpu_relax();
   }
}

}  // namespace cortos
