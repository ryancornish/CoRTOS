// Just for debugging ;)
#ifndef DEBUG_PRINT_HPP
#define DEBUG_PRINT_HPP

#include <cstdint>
#include <cstdio>
#include <format>
#include <utility>

extern "C" uint32_t port_tick_now(void);

namespace cortos::debug
{
   enum class Channel
   {
      Scheduler,
      Port,
      Thread,
      Sync,
      Test
   };

#if DEBUG_PRINT_ENABLE
   // Simple ANSI colour table
   inline const char* color(Channel ch) noexcept
   {
      switch (ch) {
         case Channel::Scheduler: return "\x1b[36m"; // cyan
         case Channel::Port:      return "\x1b[35m"; // magenta
         case Channel::Thread:    return "\x1b[34m"; // blue
         case Channel::Sync:      return "\x1b[33m"; // yellow
         case Channel::Test:      return "\x1b[32m"; // green
      }
      return "\x1b[0m";
   }

   inline const char* label(Channel ch) noexcept
   {
      switch (ch) {
         case Channel::Scheduler: return "SCHED ";
         case Channel::Port:      return "PORT  ";
         case Channel::Thread:    return "THREAD";
         case Channel::Sync:      return "SYNC  ";
         case Channel::Test:      return "TEST  ";
      }
      return "????";
   }

   inline constexpr const char* reset() noexcept { return "\x1b[0m"; }

   template <typename... Args>
   inline void print(Channel ch, const char* fmt, Args... args)
   {
      // prefix with tick + channel label
      std::printf("%s[tick=%06u][%s] ",
                  color(ch),
                  port_tick_now(),
                  label(ch));
      if constexpr (sizeof...(args) == 0) std::printf("%s", fmt);
      else std::printf(fmt, args...);
      std::printf("%s\n", reset());
   }

   inline constexpr const char* state_to_str(uint8_t state) {
      switch (state) {
         case 0: return "Ready";
         case 1: return "Running";
         case 2: return "Sleeping";
         case 3: return "Blocked";
         case 4: return "Terminated";
         default: return "???";
      }
   }

   // Forward declare the potentially-there matrix printer
   [[maybe_unused]] static void print_ready_matrix();

#endif

}

// Convenience macros
#if DEBUG_PRINT_ENABLE
#  define LOG_SCHED(fmt, ...)       cortos::debug::print(cortos::debug::Channel::Scheduler, fmt, ##__VA_ARGS__)
#  define LOG_PORT(fmt, ...)        cortos::debug::print(cortos::debug::Channel::Port,      fmt, ##__VA_ARGS__)
#  define LOG_THREAD(fmt, ...)      cortos::debug::print(cortos::debug::Channel::Thread,    fmt, ##__VA_ARGS__)
#  define LOG_SYNC(fmt, ...)        cortos::debug::print(cortos::debug::Channel::Sync,      fmt, ##__VA_ARGS__)
#  define LOG_TEST(fmt, ...)        cortos::debug::print(cortos::debug::Channel::Test,      fmt, ##__VA_ARGS__)
#  define LOG_SCHED_READY_MATRIX()  cortos::debug::print_ready_matrix();
#  define STATE_TO_STR(state)       cortos::debug::state_to_str(std::to_underlying(state))
   inline void* ptr_suffix(void* ptr) { return (void*)((uintptr_t)ptr % 10000); }
#  define TRUE_FALSE(what) what ? "TRUE" : "FALSE"
#else
#  define LOG_SCHED(...)  ((void)0)
#  define LOG_PORT(...)   ((void)0)
#  define LOG_THREAD(...) ((void)0)
#  define LOG_SYNC(...)   ((void)0)
#  define LOG_TEST(...)   ((void)0)
#  define LOG_SCHED_READY_MATRIX() ((void)0)
#  define STATE_TO_STR(state)
#  define TRUE_FALSE(what)

#endif

#endif
