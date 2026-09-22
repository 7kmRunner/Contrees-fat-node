#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <utility>

#include "lib/common/types.hpp"
#include "node.hpp"
#include "build.hpp"

namespace betree {

struct rebuild_record {
  uint64_t key;
  uint64_t value;
  uint64_t version;
  bool is_delta;
};

/*
 * A full checkpoint cannot copy only the base leaf arrays: updates buffered
 * in a fat leaf are part of the snapshot as well.  Merge one leaf at a time
 * so the rebuild retains the original O(number of records) auxiliary-memory
 * shape instead of keeping a second, tree-sized version-record vector.
 *
 * The tree's separator invariant assigns every delta key to the same key
 * range as its base leaf, so sorted leaf-local output remains globally
 * sorted.  Readers do not use delta_block::count; acquire-loading each slot
 * version follows the same publication protocol as find/scan.
 */
static inline void unfold_elems(cnodeptr t, vec<kv>& elems,
                                uint64_t snapshot_version) {
  if (t->type != LEAF) {
    for (uint8_t i = 0; i <= t->size; ++i) {
      unfold_elems(t->chs[i], elems, snapshot_version);
    }
    return;
  }

  static constexpr uint8_t MAX_REBUILD_RECORDS =
    (2*B-1) + MAX_FAT_SLOTS;
  std::array<rebuild_record, MAX_REBUILD_RECORDS> records;
  uint8_t count = 0;

  for (uint8_t i = 0; i < t->size; ++i) {
    records[count++] = {t->keys[i], t->vals[i], t->ver, false};
  }

  if (const delta_block* block = leaf_delta(t); block != nullptr) {
    for (uint8_t i = 0; i < block->capacity; ++i) {
      const delta_slot& slot = block->slots()[i];
      const uint64_t version =
        slot.version.load(std::memory_order_acquire);
      if (version != 0 && version <= snapshot_version) {
        records[count++] = {slot.key, slot.value, version, true};
      }
    }
  }

  std::sort(records.begin(), records.begin()+count,
    [](const rebuild_record& lhs, const rebuild_record& rhs) {
      if (lhs.key != rhs.key) { return lhs.key < rhs.key; }
      if (lhs.version != rhs.version) { return lhs.version < rhs.version; }
      /* A delta is the update if a malformed input repeats a version. */
      return lhs.is_delta < rhs.is_delta;
    });

  for (uint8_t begin = 0; begin < count; ) {
    uint8_t end = begin + 1;
    while (end < count && records[end].key == records[begin].key) {
      ++end;
    }
    const rebuild_record& newest = records[end-1];
    elems.emplace_back(newest.key, newest.value);
    begin = end;
  }
}

static inline void unfold_elems(cnodeptr t, vec<kv>& elems) {
  unfold_elems(t, elems, std::numeric_limits<uint64_t>::max());
}

static inline nodeptr rebuild_entire(nodeptr t, uint8_t h,
                                     uint64_t snapshot_version) {
  vec<kv> elems;
  unfold_elems(t, elems, snapshot_version);
  return build_sorted(elems.size(), h, elems.data());
}

static inline nodeptr rebuild_entire(nodeptr t, uint8_t h) {
  return rebuild_entire(t, h, std::numeric_limits<uint64_t>::max());
}

void unfold_extra(nodeptr t, vec<uint64_t>& keys, vec<nodeptr>& chs) {
  if (t->height == 0) {
    chs.push_back(t);
    return; }
  for (uint8_t i = 0; i < t->size; i++) {
    unfold_extra(t->chs[i], keys, chs);
    keys.push_back(t->keys[i]); }
  unfold_extra(t->chs[t->size], keys, chs);
}

void unfold_upper_part(nodeptr t, vec<uint64_t>& keys, vec<nodeptr>& chs) {
  if (t->type != BOUNDARY) {
    for (uint8_t i = 0; i < t->size; i++) {
      unfold_upper_part(t->chs[i], keys, chs);
      keys.push_back(t->keys[i]); }
    unfold_upper_part(t->chs[t->size], keys, chs); }
  else {
    for (uint8_t i = 0; i < t->size; i++) {
      unfold_extra(t->chs[i], keys, chs);
      keys.push_back(t->keys[i]); }
    unfold_extra(t->chs[t->size], keys, chs); }
}

static inline nodeptr rebuild_internal(nodeptr t, uint8_t h) {
  vec<uint64_t> keys;
  vec<nodeptr> chs;
  keys.push_back(0);
  unfold_upper_part(t, keys, chs);
  return std::get<0>(build_internal(h, keys, chs));
}

static inline bool leaf_over_boundary(nodeptr t, uint8_t h) {
  for (uint8_t i = 0; i < h; i++) {
    if (t->type == LEAF) { return true; }
    t = t->chs[0]; }
  return t->type == LEAF;
}

static inline nodeptr rebuild(nodeptr t, uint8_t h,
                              uint64_t snapshot_version) {
  return leaf_over_boundary(t, h)
    ? rebuild_entire(t, h, snapshot_version)
    : rebuild_internal(t, h);
}

static inline nodeptr rebuild(nodeptr t, uint8_t h) {
  return rebuild(t, h, std::numeric_limits<uint64_t>::max());
}

template <typename Visitor>
void visit_rebuilt_entire(nodeptr t, Visitor& visit) {
  if (t->type != LEAF) {
    for (uint8_t i = 0; i <= t->size; ++i) {
      visit_rebuilt_entire(t->chs[i], visit);
    }
  }
  visit(t);
}

template <typename Visitor>
void visit_rebuilt_extra(nodeptr t, Visitor& visit) {
  /* Height-zero subtrees are shared with the rebuilt root. */
  if (t->height == 0) { return; }
  for (uint8_t i = 0; i <= t->size; ++i) {
    visit_rebuilt_extra(t->chs[i], visit);
  }
  visit(t);
}

template <typename Visitor>
void visit_rebuilt_upper(nodeptr t, Visitor& visit) {
  if (t->type != BOUNDARY) {
    for (uint8_t i = 0; i <= t->size; ++i) {
      visit_rebuilt_upper(t->chs[i], visit);
    }
  } else {
    for (uint8_t i = 0; i <= t->size; ++i) {
      visit_rebuilt_extra(t->chs[i], visit);
    }
  }
  visit(t);
}

template <typename Visitor>
void visit_checkpoint_retired(nodeptr t, Visitor& visit) {
  const uint8_t h = t->verge;
  if (leaf_over_boundary(t, h)) {
    visit_rebuilt_entire(t, visit);
  } else {
    visit_rebuilt_upper(t, visit);
  }
}

static inline std::pair<uint64_t, uint64_t>
checkpoint_retired_stats(nodeptr t) {
  uint64_t nodes = 0;
  uint64_t bytes = 0;
  auto count = [&](nodeptr node) {
    ++nodes;
    bytes += node_size(node);
  };
  visit_checkpoint_retired(t, count);
  return {nodes, bytes};
}

static inline void free_checkpoint(nodeptr t) {
  auto release = [](nodeptr node) { free_node(node); };
  visit_checkpoint_retired(t, release);
}

}
