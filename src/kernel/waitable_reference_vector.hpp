#ifndef CORTOS_WAITABLE_REFERENCE_VECTOR_HPP
#define CORTOS_WAITABLE_REFERENCE_VECTOR_HPP

#include <cortos/kernel/waitable.hpp>
#include <cortos/port/port.h>

#include <array>
#include <algorithm>

namespace cortos
{

template<std::size_t Max>
class WaitableRefVector
{
private:
   std::array<Waitable*, Max> store{};
   std::size_t count = 0;

public:
   constexpr WaitableRefVector() = default;
   using iterator = Waitable**;
   using const_iterator = Waitable* const*;

   iterator begin()
   {
      return store.data();
   }

   iterator end()
   {
      return store.data() + count;
   }

   [[nodiscard]] bool empty() const noexcept
   {
      return count == 0;
   }

   [[nodiscard]] size_t size() const noexcept
   {
      return count;
   }

   [[nodiscard]] const_iterator begin() const noexcept
   {
      return store.data();
   }

   [[nodiscard]] const_iterator end() const noexcept
   {
     return store.data() + count;
   }

   [[nodiscard]] constexpr size_t capacity() const noexcept
   {
     return store.size();
   }

   Waitable* const& operator[](std::size_t index) const noexcept
   {
      CORTOS_ASSERT(index < count);
      return store[index];
   }

   void push(Waitable* waitable)
   {
      CORTOS_ASSERT(count < store.size());
      store[count++] = waitable;
   }

   void push_range(std::span<Waitable* const> waitables)
   {
      CORTOS_ASSERT(count + waitables.size() <= store.size());
      std::ranges::copy(waitables, store.data() + count);
      count += waitables.size();
   }

   void pop()
   {
      CORTOS_ASSERT(!empty());
      --count;
   }

   void sort_by_address()
   {
      if (count < 2) return;
      std::ranges::sort(begin(), end());
   }

   void clear() noexcept
   {
      count = 0;
   }
};

} // namespace cortos

#endif // CORTOS_WAITABLE_REFERENCE_VECTOR_HPP
