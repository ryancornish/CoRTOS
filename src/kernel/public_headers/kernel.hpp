/**
 * @file kernel.hpp
 * @brief CoRTOS Kernel API
 *
 * This is the main kernel header. It contains all kernel primitives and APIs.
 */

#ifndef CORTOS_KERNEL_HPP
#define CORTOS_KERNEL_HPP

#include <cortos/kernel/function.hpp>
#include <cortos/kernel/spin_lock.hpp>
#include <cortos/kernel/thread.hpp>
#include <cortos/kernel/waitable.hpp>

#include <cstdint>
#include <span>


namespace cortos::kernel
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

} // namespace cortos::kernel

#endif // CORTOS_KERNEL_HPP
