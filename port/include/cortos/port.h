/**
 * @file port.h
 * @brief CoRTOS Port Layer API (C ABI)
 *
 * This is the hardware abstraction layer between the CoRTOS kernel and
 * platform-specific code. All functions use C linkage for easy implementation
 * in assembly or C.
 *
 * Port implementations must provide all functions declared here.
 */

#ifndef CORTOS_PORT_H
#define CORTOS_PORT_H

#include "cortos/port_traits.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Port Configuration
 * ========================================================================= */

#ifndef CORTOS_PORT_SIMULATION
# define CORTOS_PORT_SIMULATION 0
#endif

/* ============================================================================
 * Type Definitions
 * ========================================================================= */

/**
 * @brief Opaque context structure (platform-specific size/alignment)
 *
 * Each port defines the actual structure. The kernel treats this as opaque.
 */
typedef struct cortos_port_context cortos_port_context_t;

/**
 * @brief Port->Kernel reschedule hook
 */
typedef void (*cortos_port_reschedule_t)(void);

/**
 * @brief Thread entry point signature
 */
typedef void (*cortos_port_entry_t)(void* arg);

/**
 * @brief Core entry point signature
 */
typedef void (*cortos_port_core_entry_t)(void);

/**
 * @brief ISR signature
 */
typedef void (*cortos_port_isr_handler_t)(void* arg);

/* ============================================================================
 * Platform Initialization
 * ========================================================================= */

/**
 * @brief Initialize the port layer
 */
void cortos_port_init(void);

/* ============================================================================
 * Critical Sections (Interrupt Control)
 * ========================================================================= */

/**
 * @brief Disable interrupts
 *
 * In simulation, this may be a no-op or track nesting depth.
 */
void cortos_port_disable_interrupts(void);

/**
 * @brief Enable interrupts
 */
void cortos_port_enable_interrupts(void);

/**
 * @brief Check if interrupts are currently enabled
 * @return true if interrupts are enabled, false otherwise
 */
bool cortos_port_interrupts_enabled(void);

/**
 * @brief Save interrupt state and disable interrupts
 * @return Previous interrupt state
 */
uint32_t cortos_port_irq_save(void);

/**
 * @brief Restore interrupt state
 * @param state Previous state returned by cortos_port_irq_save()
 */
void cortos_port_irq_restore(uint32_t state);

/* ============================================================================
 * Context Management & Switching
 * ========================================================================= */

/**
 * @brief Initialize a thread context
 * @param context Pointer to context structure (pre-allocated by kernel)
 * @param stack_base Pointer to the base (lowest address) of the stack
 * @param stack_size Size of the stack in bytes
 * @param entry Thread entry point function
 * @param arg Argument to pass to entry function
 *
 * This function sets up the context so that when port_switch() is called
 * with this context, the thread starts executing at entry(arg).
 */
void cortos_port_context_init(cortos_port_context_t* context,
                              void* stack_base,
                              size_t stack_size,
                              cortos_port_entry_t entry,
                              void* arg);

/**
 * @brief Destroy a thread context
 * @param context Pointer to context to destroy
 *
 * Called when a thread exits. Allows the port to clean up any resources.
 */
void cortos_port_context_destroy(cortos_port_context_t* context);

/**
 * @brief Switch from one context to another
 * @param from Context to save (can be NULL for first switch)
 * @param to Context to restore and resume
 *
 * Saves the current CPU state into 'from' and loads the state from 'to'.
 * Execution resumes in 'to' context.
 */
void cortos_port_switch(cortos_port_context_t* from, cortos_port_context_t* to);

/**
 * @brief Start executing the first thread
 * @param first First thread context to run
 *
 * This is called once at scheduler startup to begin execution.
 * Unlike port_switch(), there's no "from" context to save.
 */
void cortos_port_start_first(cortos_port_context_t* first);

/**
 * @brief Request a reschedule
 *
 * Yields back to the scheduler/dispatcher from the current context.
 */
void cortos_port_pend_reschedule(void);

/**
 * @brief Thread exit handler
 *
 * Called when a thread's entry function returns.
 * Should never return.
 */
void cortos_port_thread_exit(void);// __attribute__((noreturn));

/* ============================================================================
 * SMP & Multi-Core Support
 *
 * Each pthread represents a simulated "core". Core 0 runs on the calling
 * thread, additional cores spawn as pthreads.
 * ========================================================================= */

/**
 * @brief Get the ID of the current CPU core
 * @return Core ID (0-indexed)
 *
 * For single-core systems, always returns 0.
 * For SMP systems, returns which core is executing this code.
 */
uint32_t cortos_port_get_core_id(void);

/**
 * @brief Start (or release) all secondary cores and run entry on every core.
 * @param cores_to_use Number of cores to start
 * @param entry Entry point to run on each core
 *
 * After this call returns on the bootstrap core:
 *  - On embedded: typically never returns because entry will start the first thread.
 *  - On simulation: may return if port_start_first returns (cooperative).
 */
void cortos_port_start_cores(size_t cores_to_use, cortos_port_core_entry_t entry);

/**
 * @brief Send an IPI to another core to trigger a reschedule
 * @param core_id Target core ID
 */
void cortos_port_send_reschedule_ipi(uint32_t core_id);

/**
 * @brief Callback when a core's entry function returns
 *
 * Likely no-op on real targets.
 */
void cortos_port_on_core_returned(void);

/* ============================================================================
 * Thread-Local Storage
 * ========================================================================= */

/**
 * @brief Set the TLS pointer for the current thread
 * @param tls_base Pointer to the thread's TLS block
 */
void cortos_port_set_tls_pointer(void* tls_base);

/**
 * @brief Get the current TLS pointer
 * @return Current thread's TLS base pointer
 */
void* cortos_port_get_tls_pointer(void);



/* ============================================================================
 * Time Driver Port
 *
 * Provides monotonic time for real drivers (periodic / tickless) in unit
 * tests, plus tickless one-shot arming and ISR delivery when pumped.
 *
 * Note:
 * - SimulationTimeDriver owns time and does NOT use this.
 * - Periodic driver unit tests call driver.on_timer_isr() directly.
 * ========================================================================= */

/**
 * @brief Configure the underlying timer peripheral(s) used for OS time.
 * @param tick_hz Tick frequency (0 for tickless mode)
 *
 * If tick_hz > 0:
 *   Configure a periodic timer interrupt at tick_hz.
 *   The port must deliver the registered ISR handler once per tick IRQ.
 *
 * If tick_hz == 0:
 *   Configure tickless one-shot mode.
 *   The driver will call cortos_port_time_arm()/disarm() to schedule deadlines.
 *   The port must deliver the registered ISR handler when time_now() >= armed deadline.
 *
 * Called by the selected TimeDriver during start().
 */
void cortos_port_time_setup(uint32_t tick_hz);

/**
 * @brief Monotonic time source
 * @return Current time in port ticks
 *
 * Must be monotonic 64-bit in "port ticks" (opaque unit for whole system).
 */
uint64_t cortos_port_time_now(void);

/**
 * @brief Free-running counter frequency in Hz (ticks per second).
 * @return Frequency in Hz
 *
 * For example:
 *  - DWT_CYCCNT at CPU clock: 168'000'000
 *  - Timer running at 1 MHz: 1'000'000
 *  - Linux steady_clock ns ticks: 1'000'000'000
 */
uint64_t cortos_port_time_freq_hz(void);

/**
 * @brief Reset any internal global time tracking state.
 * @param time Initial time value
 *
 * On embedded targets this is typically meaningless or implemented
 * implicitly by a system reset.
 *
 * Intended primarily for simulation and unit testing to provide
 * deterministic startup conditions.
 */
void cortos_port_time_reset(uint64_t time);

/**
 * @brief Register an ISR handler for timer interrupts
 * @param handler ISR callback function
 * @param arg Argument to pass to handler
 */
void cortos_port_time_register_isr_handler(cortos_port_isr_handler_t handler, void* arg);

/**
 * @brief Enable timer interrupts
 */
void cortos_port_time_irq_enable(void);

/**
 * @brief Disable timer interrupts
 */
void cortos_port_time_irq_disable(void);

/**
 * @brief Arm a one-shot interrupt for the given absolute deadline.
 * @param deadline Absolute time in port ticks
 *
 * If called multiple times before the interrupt fires, the port must ensure
 * the earliest deadline is honored (i.e., effectively min(current, deadline)).
 *
 * Must be safe to call with interrupts disabled.
 */
void cortos_port_time_arm(uint64_t deadline);

/**
 * @brief Disable any pending one-shot.
 */
void cortos_port_time_disarm(void);

/**
 * @brief Notify the time core that there is pending time work.
 * @param core_id Target core ID
 *
 * If unimplemented on a platform, it may be an empty function.
 * Used for SMP policy where non-time cores enqueue requests for the time core.
 */
void cortos_port_send_time_ipi(uint32_t core_id);

/* ============================================================================
 * CPU Hints & Idle
 * ========================================================================= */

/**
 * @brief CPU yield hint for busy-wait loops
 */
void cortos_port_cpu_relax(void);

/**
 * @brief Platform-specific idle behavior
 *
 * Called by the kernel's idle thread when no other threads are ready.
 * Can implement power-saving features or cooperative yielding.
 */
void cortos_port_idle(void);

/* ============================================================================
 * Debug & Diagnostics
 * ========================================================================= */

/**
 * @brief Internal Kernel asserts
 * The Kernel has been setup incorrectly, or has hit an internal system error
 */
void cortos_port_system_error(uintptr_t auxilary1, uintptr_t auxilary2, char const* file_optional, int line_optional) __attribute__((noreturn));

#ifdef CORTOS_PORT_SIMULATION
 #define CORTOS_PORT_CAPTURE_FILE (__FILE__)
 #define CORTOS_PORT_CAPTURE_LINE (__LINE__)
#else
 #define CORTOS_PORT_CAPTURE_FILE ""
 #define CORTOS_PORT_CAPTURE_LINE 0
#endif

#define CORTOS_ASSERT2(condition, aux1, aux2) __builtin_expect(!!(condition), 1) ? (void)0 : cortos_port_system_error((uintptr_t)(aux1), (uintptr_t)(aux2), CORTOS_PORT_CAPTURE_FILE, CORTOS_PORT_CAPTURE_LINE)
#define CORTOS_ASSERT1(condition, aux1)       CORTOS_ASSERT2(condition, aux1, 0)
#define CORTOS_ASSERT(condition)              CORTOS_ASSERT2(condition, 0, 0)
#define CORTOS_ASSERT_OP(lhs, op, rhs)        CORTOS_ASSERT2((lhs) op (rhs), lhs, rhs)
#define CORTOS_ASSERT_NULL(pointer)           CORTOS_ASSERT2(!(pointer), pointer, 0);

/**
 * @brief Trigger a breakpoint (for debugging)
 */
void cortos_port_breakpoint(void);

/**
 * @brief Get the current stack pointer value
 * @return Current stack pointer
 */
void* cortos_port_get_stack_pointer(void);

#ifdef __cplusplus
}
#endif

#endif /* CORTOS_PORT_H */
