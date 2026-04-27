#ifndef CORTOS_WAIT_SUBSYSTEM_HPP
#define CORTOS_WAIT_SUBSYSTEM_HPP

#include <cortos/kernel/waitable.hpp>
#include <cortos/config/config.hpp>
#include <cortos/port/port.h>

#include <limits>

namespace cortos
{

struct WaitGroup;

enum class ReadyAction : uint8_t
{
   None,
   Reschedule
};

/**
 * @brief Intrusive wait-queue node for a thread waiting on a Waitable
 *
 * A WaitNode represents a single thread's participation in a wait operation
 * on a specific Waitable. Nodes are allocated from a per-thread pool and are
 * linked into a Waitable's wait queue for the duration of the wait.
 *
 * Each wait_for_any() call creates one WaitNode per Waitable involved.
 * Nodes are removed and returned to the pool when the wait completes.
 */
struct WaitNode
{
   static constexpr uint8_t INVALID_INDEX = std::numeric_limits<uint8_t>::max();
   // Pool bookkeeping
   uint8_t slot{INVALID_INDEX};
   bool  active{false};

   // Intrusive links for the Waitable's waiter queue
   WaitNode* next{nullptr};
   WaitNode* prev{nullptr};

   TaskControlBlock* tcb{nullptr};
   Waitable*    waitable{nullptr};
   WaitGroup*      group{nullptr};

   // Which index in wait_for_any({span}) this node corresponds to
   uint8_t index{INVALID_INDEX};

   constexpr void reset() noexcept
   {
      active   = false;
      next = prev = nullptr;
      waitable = nullptr;
      group    = nullptr;
      index    = INVALID_INDEX;
      // tcb and slot are explicitly not reset
   }

   [[nodiscard]] constexpr bool is_enqueued() const noexcept
   {
      return waitable != nullptr;
   }

   /**
    * @brief Attempt to wake the owning thread for this wait node
    *
    * Resolves a wait_for_any() race by attempting to "win" the associated WaitGroup.
    * If this node wins, all wait nodes participating in the same WaitGroup are
    * torn down (unlinked from their Waitables and returned to the per-thread pool),
    * and the owning thread is made runnable on its pinned core.
    *
    * If the WaitGroup has already been won by another node, this call is a no-op.
    *
    * @param acquired True if the woken thread acquired a resource as part of the wake
    *                 (e.g., mutex handoff). This is recorded in the WaitGroup result.
    */
   [[nodiscard]] ReadyAction wake_thread(bool acquired) const noexcept;
};

/**
 * @brief Per-thread pool of WaitNode objects
 *
 * Each thread owns a fixed-capacity pool of WaitNodes used to represent
 * active wait operations. This avoids dynamic allocation in the kernel
 * and guarantees bounded resource usage.
 *
 * A single wait_for_any() operation may allocate multiple nodes from the
 * pool (one per Waitable). All nodes are reclaimed when the wait completes.
 */
class WaitNodePool
{
   static constexpr std::size_t N = config::MAX_WAIT_NODES;
   static constexpr uint32_t ALL_NODES_FREE = (N == 32) ? std::numeric_limits<uint32_t>::max() : (1u << static_cast<uint32_t>(N)) - 1u;
   std::array<WaitNode, N> nodes{};
   uint32_t free_mask{ALL_NODES_FREE};

public:
   static_assert(N > 0, "MAX_WAIT_NODES must be > 0");
   static_assert(N <= std::numeric_limits<uint32_t>::digits, "WaitNodePool currently supports up to 32 nodes via uint32_t mask");

   constexpr explicit WaitNodePool(TaskControlBlock* tcb) noexcept
   {
      for (std::size_t i = 0; auto& node : nodes) {
         node.tcb  = tcb;
         node.slot = i++;
      }
   }
   ~WaitNodePool() = default;

   WaitNodePool(WaitNodePool const&)            = delete;
   WaitNodePool& operator=(WaitNodePool const&) = delete;
   WaitNodePool(WaitNodePool&&)            = delete;
   WaitNodePool& operator=(WaitNodePool&&) = delete;

   void reset_all() noexcept
   {
      for (auto& node : nodes) {
         node.reset();
      }
      free_mask = ALL_NODES_FREE;
   }

   [[nodiscard]] constexpr std::size_t capacity()   const noexcept { return nodes.size(); }
   [[nodiscard]] constexpr std::size_t free_count() const noexcept { return std::popcount(free_mask); }
   [[nodiscard]] constexpr        bool empty()      const noexcept { return free_mask == 0; }

   /**
    * Allocate a node, initialize its identity fields, and return it.
    * Returns nullptr if pool exhausted.
    */
   WaitNode* alloc(WaitGroup& group, Waitable& waitable, uint8_t index) noexcept
   {
      CORTOS_ASSERT(index != WaitNode::INVALID_INDEX);

      if (free_mask == 0) return nullptr;

      uint32_t bit = std::countr_zero(free_mask);
      free_mask &= ~(1u << bit);

      auto& node = nodes[bit];

      // Node should be inactive if the mask said it was free.
      // If not, we have a bug in free()/mask management.
      CORTOS_ASSERT(!node.active);

      node.reset();
      node.active   = true;
      node.group    = &group;
      node.waitable = &waitable;
      node.index    = index;

      return &node;
   }

   /**
    * Free a node back to the pool.
    * Caller is responsible for unlinking it from any Waitable queue first.
    */
   void free(WaitNode& node) noexcept
   {
      std::ptrdiff_t index = &node - nodes.data();

      CORTOS_ASSERT1(0 <= index && static_cast<std::size_t>(index) < N, index); // Node not from this pool

      auto& n = nodes[static_cast<std::size_t>(index)];
      CORTOS_ASSERT(n.active); // If error: You are freeing an inactive node.

      CORTOS_ASSERT_OP(n.slot, ==, static_cast<uint8_t>(index));
      n.reset();
      free_mask |= (1u << static_cast<uint32_t>(index));
   }

   /**
    * Iterate active nodes (optionally filtered by group).
    * Useful for teardown: remove all nodes for a completed wait.
    */
   void for_each_active(Function<void(WaitNode&), 32, HeapPolicy::NoHeap>&& fn, WaitGroup* only_group = nullptr) noexcept
   {
      for (std::size_t i = 0; i < N; ++i) {
         auto& node = nodes[i];
         if (!node.active) continue;
         if (only_group && node.group != only_group) continue;
         fn(node);
      }
   }

   /**
    * Return pointer to node by slot index (even if inactive).
    * Primarily for debugging / assertions.
    */
   [[nodiscard]] WaitNode* at(std::size_t slot) noexcept
   {
      if (slot >= N) return nullptr;
      return &nodes[slot];
   }
};

/**
 * @brief Coordination state for a multi-wait (wait_for_any) operation
 *
 * A WaitGroup tracks the outcome of a wait_for_any() call across multiple
 * Waitables. It records which waitable won the race and whether the waking
 * thread acquired a resource.
 *
 * Exactly one signal may win the group. All other signals are ignored once
 * the group is marked complete.
 */
struct WaitGroup
{
   std::atomic<bool> done{false};
   int       winner_index{-1};
   bool          acquired{false}; // Flag for 'resource waitables' (e.g. mutex)
   uint8_t expected_count{0};     // Purely for debug/sanity checking

   void begin(uint8_t n) noexcept
   {
      done.store(false, std::memory_order_relaxed);
      winner_index   = -1;
      acquired       = false;
      expected_count = n;
   }

   // Called by signal path. Returns true if *this* call won.
   bool try_win(int index, bool acquired_) noexcept
   {
      bool expected = false;
      if (!done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
         return false;
      }
      winner_index = index;
      acquired     = acquired_;
      return true;
   }
};

} // namespace cortos

#endif // CORTOS_WAIT_SUBSYSTEM_HPP
