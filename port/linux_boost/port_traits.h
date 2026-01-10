/**
 * @file port_traits.h
 * @brief Port-specific compile-time constants
 *
 * Each port must provide this header defining:
 * - CORTOS_PORT_CONTEXT_SIZE: Size of port_context_t in bytes
 * - CORTOS_PORT_CONTEXT_ALIGN: Alignment requirement for port_context_t
 * - CORTOS_STACK_ALIGN: Stack alignment requirement
 *
 * These values are used by the kernel to allocate context storage.
 * The port implementation must static_assert that the actual sizes match.
 */

#ifndef CORTOS_PORT_TRAITS_H
#define CORTOS_PORT_TRAITS_H

/* ============================================================================
 * Boost.Context Port (Linux Simulation)
 * ========================================================================= */

/**
 * @brief Size of port_context_t structure in bytes
 *
 * Must be >= sizeof(port_context) - verified by static_assert in port implementation.
 */
#define CORTOS_PORT_CONTEXT_SIZE  48

/**
 * @brief Alignment requirement for port_context_t
 *
 * Must be >= alignof(port_context) - verified by static_assert in port implementation.
 */
#define CORTOS_PORT_CONTEXT_ALIGN 8

/**
 * @brief Stack alignment requirement in bytes
 *
 * All thread stacks must be aligned to this boundary.
 * Must be a power of two.
 */
#define CORTOS_STACK_ALIGN 16

#define CORTOS_PORT_CACHE_LINE 64

#define CORTOS_PORT_CORE_COUNT 1

#define CORTOS_PORT_SIMULATION 1

#endif // CORTOS_PORT_TRAITS_H
