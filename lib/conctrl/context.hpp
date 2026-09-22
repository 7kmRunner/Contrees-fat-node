#pragma once

#include <algorithm>
#include <cstdint>

namespace conctrl {

static inline constexpr uint8_t INIT = 0x0;
static inline constexpr uint8_t DONE = 0xf;

template <typename T>
struct context {
  static inline constexpr uint16_t RETIRED_INLINE_CAPACITY = 6;
  static inline constexpr uint8_t RETIRED_OVERFLOW_CAPACITY = 14;

  struct retired_overflow {
    retired_overflow* next;
    uint8_t count;
    T* nodes[RETIRED_OVERFLOW_CAPACITY];
  };

  uint8_t op;

  union {
    uint8_t flags;
    struct {
      uint8_t retry          : 1;
      uint8_t wait_child     : 1;
      uint8_t wait_subtree   : 1;
      uint8_t new_checkpoint : 1;
      uint8_t gc_enabled     : 1;
      uint8_t checkpoint_rebuilt : 1;
    };
  };

  alignas(uint64_t) uint32_t sno;
  uint32_t cno;

  T* root;
  T* t_cur;
  T* t_past;

  uint64_t key;
  uint64_t val;

  /*
   * Nodes physically replaced while producing this context's version.
   * Keeping this list in the update context avoids adding permanent GC
   * metadata to every tree node.  Six pointers fit in the context's existing
   * cache-line padding; unusually deep paths spill into small linked blocks.
   */
  T* retired_inline[RETIRED_INLINE_CAPACITY];
  retired_overflow* retired_overflow_head;
  uint16_t retired_count;
  uint8_t fat_slots;

  void retire_replaced(T* node) {
    if (!gc_enabled || node == nullptr) { return; }

    if (retired_count < RETIRED_INLINE_CAPACITY) {
      retired_inline[retired_count] = node;
    } else {
      if (retired_overflow_head == nullptr ||
          retired_overflow_head->count == RETIRED_OVERFLOW_CAPACITY) {
        auto block = new retired_overflow{};
        block->next = retired_overflow_head;
        retired_overflow_head = block;
      }
      retired_overflow_head->nodes[retired_overflow_head->count++] = node;
    }
    ++retired_count;
  }

  template <typename F>
  uint16_t drain_retired(F&& consume) {
    const uint16_t count = retired_count;
    const uint16_t inline_count =
      std::min<uint16_t>(count, RETIRED_INLINE_CAPACITY);
    for (uint16_t i = 0; i < inline_count; ++i) {
      consume(retired_inline[i]);
      retired_inline[i] = nullptr;
    }

    while (retired_overflow_head != nullptr) {
      retired_overflow* block = retired_overflow_head;
      retired_overflow_head = block->next;
      for (uint8_t i = 0; i < block->count; ++i) {
        consume(block->nodes[i]);
      }
      delete block;
    }
    retired_count = 0;
    return count;
  }

  void clear_retired() {
    drain_retired([](T*) {});
  }
};

}
