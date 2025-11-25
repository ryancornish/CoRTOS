// Just for debugging ;)
#ifndef DEBUG_PRINT_HPP
#define DEBUG_PRINT_HPP

#include <cstdint>
#include <cstdio>
#include <format>
#include <utility>

extern "C" uint32_t port_tick_now(void);



namespace debug
{
   enum class Channel
   {
      Scheduler,
      Port,
      Thread,
      Sync
   };

#if DEBUG_PRINT_ENABLE
   // Simple ANSI colour table
   inline const char* color(Channel ch) noexcept
   {
      switch (ch) {
         case Channel::Scheduler: return "\x1b[36m"; // cyan
         case Channel::Port:      return "\x1b[35m"; // magenta
         case Channel::Thread:    return "\x1b[32m"; // green
         case Channel::Sync:      return "\x1b[33m"; // yellow
      }
      return "\x1b[0m";
   }

   inline const char* label(Channel ch) noexcept
   {
      switch (ch) {
         case Channel::Scheduler: return "SCHED";
         case Channel::Port:      return "PORT ";
         case Channel::Thread:    return "THRD ";
         case Channel::Sync:      return "SYNC ";
      }
      return "????";
   }

   inline constexpr const char* reset() noexcept { return "\x1b[0m"; }

   template <typename... Args>
   inline void print(Channel ch, const char* fmt, Args... args)
   {
      // prefix with tick + channel label
      std::printf("%s[tick=%08u][%s] ",
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
         default: return "???";
      }
   }
#endif

}

// Convenience macros
#if DEBUG_PRINT_ENABLE
#  define LOG_SCHED(fmt, ...)  ::debug::print(::debug::Channel::Scheduler, fmt, ##__VA_ARGS__)
#  define LOG_PORT(fmt, ...)   ::debug::print(::debug::Channel::Port,      fmt, ##__VA_ARGS__)
#  define LOG_THREAD(fmt, ...) ::debug::print(::debug::Channel::Thread,    fmt, ##__VA_ARGS__)
#  define LOG_SYNC(fmt, ...)   ::debug::print(::debug::Channel::Sync,      fmt, ##__VA_ARGS__)
#  define STATE_TO_STR(state)  ::debug::state_to_str(std::to_underlying(state))
[[maybe_unused]] static void LOG_SCHED_READY_MATRIX();
   inline void* ptr_suffix(void* ptr) { return (void*)((uintptr_t)ptr % 10000); }
#  define TRUE_FALSE(what) what ? "TRUE" : "FALSE"
#else
#  define LOG_SCHED(...)  ((void)0)
#  define LOG_PORT(...)   ((void)0)
#  define LOG_THREAD(...) ((void)0)
#  define LOG_SYNC(...)   ((void)0)
#  define LOG_SCHED_READY_MATRIX() ((void)0)
#  define STATE_TO_STR(state)
#  define TRUE_FALSE(what)

#endif

#endif
