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

#include "port_traits.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Port Configuration
 * ========================================================================= */

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

/**
 * @brief Get the total number of CPU cores
 * @return Number of cores available
 */
uint32_t cortos_port_get_core_count(void);

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

void cortos_port_cpu_relax(void);

/* ============================================================================
 * Inter-Processor Interrupts (SMP Support)
 * ========================================================================= */

/**
 * @brief Send an IPI to another core to trigger a reschedule
 * @param core_id Target core ID
 */
void cortos_port_send_reschedule_ipi(uint32_t core_id);

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
 * @param tick_hz Tick frequency in Hz (e.g., 1000 for 1ms tick)
 *
 * Called once at system startup before any threads are created.
 */
void cortos_port_init(uint32_t tick_hz);

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

#ifdef __cplusplus
}
#endif

#endif /* CORTOS_PORT_H */
