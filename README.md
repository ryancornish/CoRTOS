# CoRTOS
CoRTOS is a small, modern C++ real-time kernel for embedded systems.
Being a tiny, testable kernel that is easy to reason about, port, and extend.
It's designed for speed and safety, and aims for clarity.

At its core, CoRTOS is a fixed-priority preemptive scheduler with:
- A set of minimal synchronization primitives.
- A threading API.
- A set of "opt-in" extension features.

It is not trying to be a "FreeRTOS killer". It’s closer to a precise instrument: you should be able to understand what it does, why it does it, and how it will behave under stress - without crawling through a maze of macros and #ifdefs.

## Project Goals & Philosophy
### 1. Predictability

CoRTOS aims to make scheduling decisions obvious and inspectable.

### 2. Modern C++

CoRTOS is written in modern C++ (C++23):

- Strong types instead of integer soup.
- RAII for critical sections / scheduler locks.
- Type-erased jobs instead of ad-hoc function pointers everywhere.
- Templates for policy models.
- "Pay for what you use".
- `constexpr` as much as possible - make the compiler do the heavy lifting.

### 3. Bare-metal first, simulation as a first-class citizen

The primary target are MCUs (to-be-defined which ones), but CoRTOS is built with a strong emphasis on:

- A Linux simulation backend so scheduling, timing, and synchronization logic can be tested on a desktop.
- Being able to reproduce tricky timing and priority-inversion scenarios without a logic analyser attached to a dev board.
- Keeping the core small enough that you can read it before you trust it.
- You should be able to develop and debug most kernel-level logic on your workstation, then move to the microcontroller when you're ready.

### 4. Minimal core, opt-in power

The kernel is intentionally small. The idea is:

- A tight, well-specified core: scheduler, threads, time, basic sync primitives.
- Opt-in extensions layered on top (job queues, async dispatch, optional heap, etc.).
- No global "kitchen sink" configuration header that tries to anticipate every possible use case.
- If you want more complex patterns (async job dispatch, work stealing, message passing), they should be buildable on top of the primitives, not baked into the scheduler in opaque ways.

## Who Is CoRTOS For?

Embedded developers who want:
- A small, understandable RTOS for (x, y, z) MCUs.
- Modern C++ ergonomics.

Embedded developers who like:
- Extending kernel code.
- Building bespoke systems rather than dropping in a monolithic RTOS black box.
- If you want an RTOS that feels like part of your codebase rather than an opaque dependency, CoRTOS is aimed squarely at you.

## Status

CoRTOS is an active work-in-progress, we are in early days right now:

- Core scheduler and synchronization primitives are implemented and under test.
- Simulation backend exists and is used to exercise scheduling and priority semantics.
- Job model is implemented and being integrated into higher-level patterns (job queues, async invocation).
- Ports and extension modules (e.g. optional heap, job dispatch helpers) are evolving.
- APIs may still change as the design is refined and real use cases shake out rough edges.

## High-Level Roadmap

Planned / ongoing areas:

- More polished job queues / async dispatch helpers.
- Optional heap extension (allocator tuned for CoRTOS usage patterns).
- More example projects:
  - MCU demos (blinky + real workloads).
  - Simulation examples illustrating priority inversion, condvar usage, etc.
- More ports!!!

If you're reading the source, poking at the scheduler, or wanting to port CoRTOS to an MCU, feedback and ideas are very welcome.
