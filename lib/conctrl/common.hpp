#pragma once

#include <bit>
#include <cstdint>

#include "mimalloc.h"
#include "mimalloc-new-delete.h"
#include "mimalloc-override.h"

#ifndef LIBCONCTRL_BUFFER_SIZE
#define LIBCONCTRL_BUFFER_SIZE 65536
#endif

namespace conctrl {

static inline constexpr uint BUFFER_SIZE = std::bit_ceil<uint>(LIBCONCTRL_BUFFER_SIZE);

template<typename T>
T load_consume(T const volatile* addr)
{
  return __atomic_load_n(addr, __ATOMIC_ACQUIRE);
}

template<typename T>
void store_release(T volatile* addr, T v)
{
  __atomic_store_n(addr, v, __ATOMIC_RELEASE);
}

}
