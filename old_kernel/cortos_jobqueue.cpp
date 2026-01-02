/**
 *
 */
#include "cortos.hpp"
#include <type_traits>

namespace cortos
{
   static_assert(std::is_same_v<JobQueue, extension::jobqueue::JobQueue<true>>, "This TU should not be compiled if the extension is disabled");


   // JobQueue Implementation
   void JobQueue::push(Job&& job)
   {
      {
         Mutex::Lock guard(mutex);
         if (count == buffer.size()) std::terminate();

         buffer[tail] = std::move(job);
         tail = (tail + 1) % buffer.size();
         ++count;
      }
      items.release(1);
   }

   bool JobQueue::try_push(Job&& job) noexcept
   {
      {
         Mutex::Lock guard(mutex);

         if (count == buffer.size()) return false; // caller handles back-pressure

         buffer[tail] = std::move(job);
         tail = (tail + 1) % buffer.size();
         ++count;
      }
      items.release(1);
      return true;
   }

   Job JobQueue::take()
   {
      // Block until there is at least one job.
      items.acquire();

      Mutex::Lock guard(mutex);
      // items guarantees count > 0
      auto job = std::move(buffer[head]);
      head = (head + 1) % buffer.size();
      --count;
      return job;
   }

   std::optional<Job> JobQueue::try_take() noexcept
   {
      // Try to claim an item token without blocking.
      if (!items.try_acquire()) return std::nullopt; // empty

      Mutex::Lock guard(mutex);
      // We have a token, so there must be an item.
      auto job = std::make_optional(std::move(buffer[head]));
      head = (head + 1) % buffer.size();
      --count;
      return job;
   }

} // namespace cortos
