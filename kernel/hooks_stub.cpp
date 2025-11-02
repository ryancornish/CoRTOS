// kernel/hooks_stub.cpp
#include <port.h>
#include <atomic>

static std::atomic<bool> need_resched{false};

extern "C" void rtk_on_tick(void) {
    // For now just mark that a reschedule would be nice.
    need_resched.store(true, std::memory_order_relaxed);
}

extern "C" void rtk_request_reschedule(void) {
    need_resched.store(true, std::memory_order_relaxed);
}
