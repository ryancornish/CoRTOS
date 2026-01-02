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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
* @brief Perform a context switch between threads
* @param old_sp Pointer to store the old stack pointer
* @param new_sp New stack pointer to load
*
* This function must:
* 1. Save current CPU context (registers) onto the stack
* 2. Store the resulting stack pointer to *old_sp
* 3. Load new_sp into the stack pointer register
* 4. Restore CPU context from the new stack
*
* On first call for a new thread, new_sp points to a pre-initialized stack.
*/
void cortos_port_switch_context(void** old_sp, void* new_sp);

/**
* @brief Initialize a thread's stack for first execution
* @param stack_base Pointer to the base (highest address) of the stack
* @param stack_size Size of the stack in bytes
* @param entry Thread entry point function
* @param arg Argument to pass to entry function
* @return Initial stack pointer value for this thread
*
* This function sets up the stack so that when cortos_port_switch_context()
* is called with this stack pointer, the thread starts executing at entry(arg).
*/
void* cortos_port_initialize_stack(
   void* stack_base,
   size_t stack_size,
   void (*entry)(void*),
   void* arg
);

/* ============================================================================
* Critical Sections (Interrupt Control)
* ========================================================================= */

/**
* @brief Disable interrupts and return previous state
* @return Previous interrupt enable state (opaque value)
*
* This must disable interrupts at the CPU level. The return value is
* platform-specific and will be passed back to cortos_port_restore_interrupts().
*/
uint32_t cortos_port_disable_interrupts(void);

/**
* @brief Restore interrupts to previous state
* @param state State returned from cortos_port_disable_interrupts()
*/
void cortos_port_restore_interrupts(uint32_t state);

/**
* @brief Check if interrupts are currently enabled
* @return true if interrupts are enabled, false otherwise
*/
bool cortos_port_interrupts_enabled(void);

/* ============================================================================
* Atomic Operations (SMP Support)
* ========================================================================= */

/**
* @brief Atomic compare-and-swap (32-bit)
* @param ptr Pointer to value
* @param expected Expected current value
* @param desired Value to write if *ptr == expected
* @return true if swap occurred, false otherwise
*
* If *ptr == expected, atomically sets *ptr = desired and returns true.
* Otherwise, leaves *ptr unchanged and returns false.
*/
bool cortos_port_atomic_compare_exchange_32(
   volatile uint32_t* ptr,
   uint32_t expected,
   uint32_t desired
);

/**
* @brief Atomic fetch-and-add (32-bit)
* @param ptr Pointer to value
* @param value Value to add
* @return Previous value of *ptr
*/
uint32_t cortos_port_atomic_fetch_add_32(volatile uint32_t* ptr, uint32_t value);

/**
* @brief Atomic load (32-bit)
* @param ptr Pointer to value
* @return Current value of *ptr
*/
uint32_t cortos_port_atomic_load_32(volatile uint32_t* ptr);

/**
* @brief Atomic store (32-bit)
* @param ptr Pointer to value
* @param value Value to store
*/
void cortos_port_atomic_store_32(volatile uint32_t* ptr, uint32_t value);

/**
* @brief Memory barrier (full fence)
*
* Ensures all memory operations before this fence complete before
* any memory operations after the fence begin.
*/
void cortos_port_memory_barrier(void);

/* ============================================================================
* Spinlocks (SMP Support)
* ========================================================================= */

/**
* @brief Initialize a spinlock
* @param lock Pointer to spinlock (opaque, platform-defined size)
*/
void cortos_port_spinlock_init(void* lock);

/**
* @brief Acquire a spinlock
* @param lock Pointer to spinlock
*
* Busy-waits until the lock is acquired. May disable interrupts on single-core.
*/
void cortos_port_spinlock_lock(void* lock);

/**
* @brief Release a spinlock
* @param lock Pointer to spinlock
*/
void cortos_port_spinlock_unlock(void* lock);

/**
* @brief Try to acquire a spinlock without blocking
* @param lock Pointer to spinlock
* @return true if acquired, false if already locked
*/
bool cortos_port_spinlock_trylock(void* lock);

/* ============================================================================
* Inter-Processor Interrupts (SMP Support)
* ========================================================================= */

/**
* @brief Send an IPI to another core to trigger a reschedule
* @param core_id Target core ID
*
* For single-core systems, this is a no-op.
* For SMP systems, this sends an interrupt to the target core to trigger
* the scheduler to run (e.g., when a higher-priority thread becomes ready).
*/
void cortos_port_send_reschedule_ipi(uint32_t core_id);

/* ============================================================================
* Thread-Local Storage (TLS)
* ========================================================================= */

/**
* @brief Set the TLS pointer for the current thread
* @param tls_base Pointer to the thread's TLS block
*
* This typically writes to a CPU register (e.g., ARM TPIDR, RISC-V tp).
* The compiler will use this register to access thread_local variables.
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
* Called once at system startup, before any threads are created.
* Can be used to set up platform-specific state (e.g., interrupt controllers).
*/
void cortos_port_init(void);

/* ============================================================================
* Idle Hook
* ========================================================================= */

/**
* @brief Platform-specific idle behavior
*
* Called by the kernel's idle thread when no other threads are ready.
* Can implement power-saving features (e.g., WFI on ARM, HLT on x86).
* Must return when an interrupt occurs.
*/
void cortos_port_idle(void);

/* ============================================================================
* Debug / Diagnostics
* ========================================================================= */

/**
* @brief Trigger a breakpoint (for debugging)
*
* Used by the kernel to halt execution for debugging (e.g., assertion failures).
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