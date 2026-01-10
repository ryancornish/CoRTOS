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
   std::printf("HELLO");
   while (true) {}
}

int main()
{
   // 1) init kernel
   kernel::initialise();

   // 2) provide stack storage
   // NOTE: your StackLayout assumes stack grows "down" inside provided span;
   // just make it a decent size and aligned.
   alignas(16) static std::byte stack[16 * 1024];

   // 3) create a single thread
   Thread t1(
      entry,
      std::span<std::byte>{stack, sizeof(stack)},
      Thread::Priority{0},            // 0 = highest priority in your scheme
      Core0
   );

   kernel::start();
}
