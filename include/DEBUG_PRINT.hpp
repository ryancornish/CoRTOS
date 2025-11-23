// Just for debugging ;)
#ifndef DEBUG_PRINT_HPP
#define DEBUG_PRINT_HPP

#include <cstdint>
#include <cstdio>

extern uint32_t port_tick_now(void);



namespace debug
{
   enum class Channel
   {
      Scheduler,
      Port,
      Thread,
      Sync
   };

#if DEBUG_PRINT_ENABLE && defined(RTK_SIMULATION)
   // Simple ANSI colour table (tweak to taste)
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
      std::printf(fmt, args...);
      std::printf("%s\n", reset());
   }

#else
   // No-colour / no-logging version
   template <typename... Args>
   inline void print(Channel, const char* fmt, Args... args)
   {
      std::printf("[tick=%08u] ", port_tick_now());
      std::printf(fmt, args...);
      std::printf("\n");
   }
#endif

}

// Convenience macros
#if DEBUG_PRINT_ENABLE
#  define LOG_SCHED(fmt, ...)  ::debug::print(::debug::Channel::Scheduler, fmt, ##__VA_ARGS__)
#  define LOG_PORT(fmt, ...)   ::debug::print(::debug::Channel::Port,      fmt, ##__VA_ARGS__)
#  define LOG_THREAD(fmt, ...) ::debug::print(::debug::Channel::Thread,    fmt, ##__VA_ARGS__)
#  define LOG_SYNC(fmt, ...)   ::debug::print(::debug::Channel::Sync,      fmt, ##__VA_ARGS__)
#else
#  define LOG_SCHED(...)  ((void)0)
#  define LOG_PORT(...)   ((void)0)
#  define LOG_THREAD(...) ((void)0)
#  define LOG_SYNC(...)   ((void)0)
#endif

#endif
