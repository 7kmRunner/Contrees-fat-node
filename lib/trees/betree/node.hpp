#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <tuple>

#include <mimalloc.h>
#include <mimalloc-override.h>
#include <mimalloc-new-delete.h>

#include "lib/common/arrayops.hpp"

namespace betree {

enum NODE_TYPE { UPPER, LOWER, BOUNDARY, LEAF };

static inline constexpr uint8_t B = 8;
static inline constexpr uint8_t E = 3;
static inline constexpr uint8_t MAX_FAT_SLOTS = 8;

/*
 * Leaf deltas are append-only.  The payload is initialized before version is
 * release-published; readers acquire-load version before inspecting it.
 * Version zero is reserved for an unused slot (updates start at version one).
 */
struct delta_slot {
  std::atomic_uint64_t version { 0 };
  uint64_t key { 0 };
  uint64_t value { 0 };
};

/*
 * Slots live immediately after this header in the same allocation.  Keeping
 * them out of node preserves the original 256-byte BeTree node layout and
 * lets capacities 2/4/8 pay only for the slots they request.
 */
struct alignas(delta_slot) delta_block {
  uint8_t capacity;
  uint8_t count; // single SeqCow writer only; readers never inspect count

  delta_slot* slots() {
    return reinterpret_cast<delta_slot*>(this + 1);
  }

  const delta_slot* slots() const {
    return reinterpret_cast<const delta_slot*>(this + 1);
  }
};

static_assert(sizeof(delta_block) % alignof(delta_slot) == 0);

static inline bool supported_fat_slots(uint8_t capacity) {
  return capacity == 2 || capacity == 4 || capacity == 8;
}

static inline uint64_t delta_block_size(uint8_t capacity) {
  return sizeof(delta_block) + capacity*sizeof(delta_slot);
}

static inline delta_block* new_delta_block(uint8_t capacity) {
  void* storage = malloc(delta_block_size(capacity));
  auto* block = new (storage) delta_block { capacity, 0 };
  for (uint8_t i = 0; i < capacity; ++i) {
    new (block->slots()+i) delta_slot();
  }
  return block;
}

static inline void free_delta_block(delta_block* block) {
  if (block == nullptr) { return; }
  for (uint8_t i = 0; i < block->capacity; ++i) {
    block->slots()[i].~delta_slot();
  }
  block->~delta_block();
  free(block);
}

struct alignas(128) node {
  uint32_t ver;
  uint8_t type;
  uint8_t verge;
  uint8_t size;
  uint8_t height;
  uint64_t keys[2*B-1];
  union {
    node* chs[2*B];
    /* vals[2*B-1] is leaf-only storage for a lazy delta_block pointer. */
    uint64_t vals[2*B]; };
};

static_assert(sizeof(node) == 256);

using nodeptr = node*;
using cnodeptr = const node*;

static inline nodeptr new_node(uint32_t ver, uint8_t type, uint8_t size) {
  nodeptr t = (nodeptr)aligned_alloc(128, sizeof(node));
  t->ver = ver;
  t->type = type;
  t->verge = 0;
  t->size = size;
  t->height = 0;
  return t;
}

static inline nodeptr new_leaf(uint32_t ver, uint8_t size) {
  nodeptr t = new_node(ver, LEAF, size);
  memset(t->keys+size, 0, (2*B-size-1)*sizeof(uint64_t));
  memset(t->vals+size, 0, (2*B-size)*sizeof(uint64_t));
  return t;
}

static inline nodeptr new_internal(uint32_t ver, uint8_t size) {
  nodeptr t = new_node(ver, LOWER, size);
  memset(t->keys+size, 0, (2*B-size-1)*sizeof(uint64_t));
  memset(t->chs+size+1, 0, (2*B-size-1)*sizeof(nodeptr));
  return t;
}

static inline nodeptr new_root(uint32_t ver, uint64_t key, nodeptr ch0, nodeptr ch1) {
  nodeptr t = new_internal(ver, 1);
  t->keys[0] = key;
  t->chs[0] = ch0;
  t->chs[1] = ch1;
  return t;
}

static inline nodeptr copy_leaf(nodeptr t, uint32_t ver) {
  nodeptr t_new = (nodeptr)aligned_alloc(128, sizeof(node));
  memcpy(t_new, t, sizeof(node));
  t_new->ver = ver;
  /* copy_leaf intentionally does not share ownership of a fat sidecar. */
  t_new->vals[2*B-1] = 0;
  return t_new;
}

static inline nodeptr copy_internal(nodeptr t, uint32_t ver) {
  nodeptr t_new = (nodeptr)aligned_alloc(128, sizeof(node));
  memcpy(t_new, t, offsetof(node, chs));
  t_new->ver = ver;
  return t_new;
}

static inline delta_block* leaf_delta(cnodeptr t) {
  if (t == nullptr || t->type != LEAF) { return nullptr; }
  uint64_t& bits = const_cast<uint64_t&>(t->vals[2*B-1]);
  const uint64_t value =
    std::atomic_ref<uint64_t>(bits).load(std::memory_order_acquire);
  return reinterpret_cast<delta_block*>(static_cast<uintptr_t>(value));
}

static inline void publish_leaf_delta(nodeptr t, delta_block* block) {
  const uint64_t value = static_cast<uint64_t>(
    reinterpret_cast<uintptr_t>(block));
  std::atomic_ref<uint64_t>(t->vals[2*B-1])
    .store(value, std::memory_order_release);
}

static inline uint8_t leaf_delta_capacity(cnodeptr t) {
  const delta_block* block = leaf_delta(t);
  return block == nullptr ? 0 : block->capacity;
}

static inline uint8_t leaf_delta_count(cnodeptr t) {
  const delta_block* block = leaf_delta(t);
  if (block == nullptr) { return 0; }

  uint8_t count = 0;
  for (uint8_t i = 0; i < block->capacity; ++i) {
    if (block->slots()[i].version.load(std::memory_order_acquire) != 0) {
      ++count;
    }
  }
  return count;
}

static inline bool append_leaf_delta(nodeptr t, uint8_t capacity,
                                     uint64_t version, uint64_t key,
                                     uint64_t value) {
  if (!supported_fat_slots(capacity)) { return false; }

  delta_block* block = leaf_delta(t);
  if (block == nullptr) {
    block = new_delta_block(capacity);
    publish_leaf_delta(t, block);
  }

  /* A controller keeps one capacity for the lifetime of a tree. */
  if (block->capacity != capacity || block->count >= block->capacity) {
    return false;
  }

  delta_slot& slot = block->slots()[block->count];
  slot.key = key;
  slot.value = value;
  slot.version.store(version, std::memory_order_release);
  ++block->count;
  return true;
}

static inline uint64_t node_size(cnodeptr t) {
  const delta_block* block = leaf_delta(t);
  return sizeof(node) + (block == nullptr ? 0 : delta_block_size(block->capacity));
}

static inline void free_node(nodeptr t) {
  free_delta_block(leaf_delta(t));
  free(t);
}

static inline std::tuple<uint64_t, nodeptr, nodeptr> split(nodeptr t, uint32_t ver) {
  nodeptr t_l = new_internal(ver, B-1);
  nodeptr t_r = new_internal(ver, B-1);
  t_l->height = t->height;
  t_r->height = t->height;
  copy(t_l->keys, t->keys, B-1);
  copy(t_r->keys, t->keys+B, B-1);
  // chs are copied at next stage
  return std::make_tuple(t->keys[B-1], t_l, t_r);
}

static inline void copych(nodeptr t_new, nodeptr t_old) {
  copy(t_new->chs, t_old->chs, 2*B);
}

static inline void copych_split(nodeptr* ts, nodeptr t_old) {
  copy_split(ts[0]->chs, ts[1]->chs, t_old->chs, 2*B, B);
}

static inline uint8_t findch(cnodeptr t, uint64_t key) {
  uint8_t i = 0;
  for ( ; i < t->size; i++) { if (t->keys[i] > key) { break; } }
  return i;
}

static inline uint8_t findval(cnodeptr t, uint64_t key) {
  uint8_t i = 0;
  for ( ; i < t->size; i++) { if (t->keys[i] >= key) { break; } }
  return i;
}

static inline void insert_child(nodeptr t, uint64_t key, nodeptr t_l, nodeptr t_r, uint8_t pos) {
  insert(t->keys, t->size, pos, key);
  t->chs[pos] = t_l;
  insert(t->chs, t->size+1, pos+1, t_r);
  t->size++;
}

}
