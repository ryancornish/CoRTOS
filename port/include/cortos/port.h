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


/**
 * @brief Opaque context structure (platform-specific size/alignment)
 *
 * Each port defines the actual structure. The kernel treats this as opaque.
 */
typedef struct cortos_port_context cortos_port_context_t;

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
 * Core Identification (SMP Support)
 * ========================================================================= */

/**
 * @brief Get the ID of the current CPU core
 * @return Core ID (0-indexed)
 *
 * For single-core systems, always returns 0.
 * For SMP systems, returns which core is executing this code.
 */
uint32_t cortos_port_get_core_id(void);

/* ============================================================================
 * Context Switching
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
 * @brief Yield from current thread back to scheduler
 *
 * Called by a running thread to voluntarily give up the CPU.
 * The scheduler will decide which thread to run next.
 */
void cortos_port_yield(void);

/**
 * @brief Thread exit handler
 *
 * Called when a thread's entry function returns.
 * Should never return.
 */
void cortos_port_thread_exit(void) __attribute__((noreturn));

void cortos_port_pend_reschedule(void);

/**
 * @brief Enter the scheduler context for this core and run its loop.
 *
 * The port must switch into a scheduler-owned execution context.
 * This function must not return.
 *
 * On embedded ports this would typically enable interrupts and start the first thread.
 * On the Boost port it starts the scheduler fiber for this pthread/core.
 */
void cortos_port_run_scheduler(void) __attribute__((noreturn));

/* ============================================================================
 * Critical Sections (Interrupt Control)
 * ========================================================================= */

uint32_t cortos_port_irq_save(void);

void cortos_port_irq_restore(uint32_t state);

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

void cortos_port_cpu_relax(void);

/* ============================================================================
 * Inter-Processor Interrupts (SMP Support)
 * ========================================================================= */

/**
 * @brief Send an IPI to another core to trigger a reschedule
 * @param core_id Target core ID
 */
void cortos_port_send_reschedule_ipi(uint32_t core_id);

/**
 * @brief Start (or release) all secondary cores and run entry on every core.
 *
 * After this call returns on the bootstrap core:
 *  - On embedded: typically never returns because entry will start the first thread.
 *  - On simulation: may return if port_start_first returns (cooperative).
 */
void cortos_port_start_cores(cortos_port_core_entry_t entry);

// Likely no-op on real targets.
void cortos_port_on_core_returned();


/* ============================================================================
 * Thread-Local Storage (TLS)
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
 * Platform Initialization
 * ========================================================================= */

/**
 * @brief Initialize the port layer
 *
 * Called once at system startup before any threads are created.
 */
void cortos_port_init(void);

/* ============================================================================
 * Idle Hook
 * ========================================================================= */

/**
 * @brief Platform-specific idle behavior
 *
 * Called by the kernel's idle thread when no other threads are ready.
 * Can implement power-saving features or cooperative yielding.
 */
void cortos_port_idle(void);

/* ============================================================================
 * Debug / Diagnostics
 * ========================================================================= */

/**
 * @brief Trigger a breakpoint (for debugging)
 */
void cortos_port_breakpoint(void);

/**
 * @brief Get the current stack pointer value
 * @return Current stack pointer
 */
void* cortos_port_get_stack_pointer(void);

/* ============================================================================
 * Time Driver Port
 * ========================================================================= */

/**
 * @brief Configure the underlying timer peripheral(s) used for OS time.
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
 * Must be monotonic 64-bit in "port ticks" (opaque unit for whole system).
 */
uint64_t cortos_port_time_now(void);

/**
 * @brief Free-running counter frequency in Hz (ticks per second).
 *
 * For example:
 *  - DWT_CYCCNT at CPU clock: 168'000'000
 *  - Timer running at 1 MHz: 1'000'000
 *  - Linux steady_clock ns ticks: 1'000'000'000
 */
uint64_t cortos_port_time_freq_hz(void);

/**
 * @brief Arm a one-shot interrupt for the given absolute deadline.
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
 * @brief Interrupt enable/disable
 */
void     cortos_port_time_irq_enable(void);
void     cortos_port_time_irq_disable(void);

void cortos_port_time_register_isr_handler(cortos_port_isr_handler_t handler, void* arg);

/**
 * @brief Optional: notify the time core that there is pending time work.
 *
 * If unimplemented on a platform, it may be an empty function.
 * Used for SMP policy where non-time cores enqueue requests for the time core.
 */
void cortos_port_send_time_ipi(uint32_t core_id);

/**
 * @brief Optional: reset any internal global time tracking state.
 *
 * On embedded targets this is typically meaningless or implemented
 * implicitly by a system reset.
 *
 * Intended primarily for simulation and unit testing to provide
 * deterministic startup conditions.
 */
void cortos_port_time_reset(uint64_t time);


#ifdef __cplusplus
}
#endif

#endif /* CORTOS_PORT_H */
