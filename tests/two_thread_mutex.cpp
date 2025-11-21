#include <cornishrtk.hpp>
#include <array>
#include <cstdint>
#include <iostream>

alignas(16) std::array<std::byte, 8 * 1024> t1_stack{};
alignas(16) std::array<std::byte, 8 * 1024> t2_stack{};

// Shared state
constinit static rtk::Mutex mutex;
static std::uint32_t counter = 0;

// Slightly different sleep periods so we see interleaving
constexpr std::uint32_t T1_SLEEP_TICKS = 15;
constexpr std::uint32_t T2_SLEEP_TICKS = 20;

static void worker(void* arg)
{
   const char* name = static_cast<const char*>(arg);

   while (true) {
      // --- Critical section ---
      mutex.lock();
      auto my_id = rtk::Scheduler::tick_now().value(); // just to show something
      std::uint32_t local = ++counter;

      std::cout << "[" << name << "] acquired mutex. Counter=" << local << ", tick=" << my_id << "\n";

      // Simulate some work while holding the lock
      rtk::Scheduler::sleep_for(5);

      std::cout << "[" << name << "] releasing mutex. Counter=" << local << "\n";
      mutex.unlock();
      // --- End critical section ---

      // Now sleep outside the lock so the other thread gets a turn.
      if (std::string_view{name} == "T1") {
         rtk::Scheduler::sleep_for(T1_SLEEP_TICKS);
      } else {
         rtk::Scheduler::sleep_for(T2_SLEEP_TICKS);
      }
   }
}

int main()
{
   rtk::Scheduler::init(5);

   rtk::Thread t1(rtk::Thread::Entry(worker, (void*)"T1"), t1_stack, rtk::Thread::Priority(1));

   rtk::Thread t2(rtk::Thread::Entry(worker, (void*)"T2"), t2_stack, rtk::Thread::Priority(2));

   rtk::Scheduler::start();
   return 0;
}
