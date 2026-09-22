#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace contrees {
// Append-only, single writer. Zero denotes an unpublished record.
struct fat_value {
  struct record { uint64_t value; uint32_t version; };
  struct overflow {
    uint64_t capacity;
    record* records() { return reinterpret_cast<record*>(this + 1); }
  };
  uint64_t values[2];
  uint32_t versions[2];
  overflow* extra;

  template<class T> static T acquire(const T& x) {
    return std::atomic_ref<T>(const_cast<T&>(x)).load(std::memory_order_acquire);
  }
  uint64_t read(uint64_t base, uint64_t snapshot) const {
    uint64_t best = 0;
    for (unsigned i=0; i<2; ++i) {
      auto v = acquire(versions[i]);
      if (v && v <= snapshot && v > best) { best=v; base=values[i]; }
    }
    auto p = acquire(extra);
    if (p) for (unsigned i=0; i<p->capacity; ++i) {
      auto v = acquire(p->records()[i].version);
      if (v && v <= snapshot && v > best) { best=v; base=p->records()[i].value; }
    }
    return base;
  }
  bool append(uint8_t slots, uint32_t version, uint64_t value) {
    if (!version || (slots != 2 && slots != 4 && slots != 8)) return false;
    for (unsigned i=0; i<2; ++i) if (!acquire(versions[i])) {
      values[i]=value;
      std::atomic_ref<uint32_t>(versions[i]).store(version, std::memory_order_release);
      return true;
    }
    if (slots == 2) return false;
    auto p=acquire(extra);
    if (!p) {
      // Exact capacity, no unused six-record allocation in four-slot mode.
      p=static_cast<overflow*>(::calloc(1, bytes(slots-2)));
      if (!p) throw std::bad_alloc();
      p->capacity=slots-2;
      std::atomic_ref<overflow*>(extra).store(p, std::memory_order_release);
    }
    for (unsigned i=0; i<p->capacity; ++i) if (!acquire(p->records()[i].version)) {
      p->records()[i].value=value;
      std::atomic_ref<uint32_t>(p->records()[i].version).store(version, std::memory_order_release);
      return true;
    }
    return false;
  }
  static uint64_t bytes(uint64_t capacity) { return sizeof(uint64_t)+capacity*sizeof(record); }
  uint64_t allocated_bytes() const { auto p=acquire(extra); return p ? bytes(p->capacity) : 0; }
  void destroy() { ::free(extra); }
};
static_assert(sizeof(fat_value)==32);
}
