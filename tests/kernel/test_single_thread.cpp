#include "cortos/kernel.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace cortos;

static void entry()
{
   while (true) {
      std::printf("core %d: entry()\n", this_thread::core_id());
      struct timespec req = {.tv_sec = 1, .tv_nsec = 1'000'000};
      nanosleep(&req, nullptr);
   }
}

int main()
{
   kernel::initialise();

   alignas(16) static std::array<std::byte, 16 * 1024> stack;

   Thread t1(
      entry,
      stack,
      Thread::Priority(0),
      Core0
   );

   kernel::start();
}
