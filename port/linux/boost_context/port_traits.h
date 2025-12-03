/**
 * port_traits.h
 * Port traits for Linux Boost.Context port backend.
 */
#ifndef PORT_TRAITS_H
#define PORT_TRAITS_H

#define CORTOS_SIMULATION 1

#define CORTOS_PORT_CONTEXT_SIZE   48u   // Must match sizeof(port_context)
#define CORTOS_PORT_CONTEXT_ALIGN  8u    // Must match alignof(port_context)
#define CORTOS_STACK_ALIGN         16u   // x86-64 SysV stack must be 16-byte aligned
#define CORTOS_TLS_SIZE            0u    // TLS unmanaged in the Linux sim
#define CORTOS_TLS_ALIGN           alignof(std::max_align_t)

#endif
