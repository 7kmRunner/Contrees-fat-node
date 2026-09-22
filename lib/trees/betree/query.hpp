#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

#include "lib/common/types.hpp"
#include "node.hpp"

namespace betree {

static inline opt<uint64_t> find(cnodeptr t, uint64_t key,
                                 uint64_t snapshot_version) {
  if (t == nullptr) {
    return std::nullopt;
  }
  while (t->type != LEAF) {
    t = t->chs[findch(t, key)];
  }

  opt<uint64_t> result = std::nullopt;
  uint64_t result_version = 0;
  const uint8_t i = findval(t, key);
  if (i < t->size && t->keys[i] == key) {
    result = std::make_optional(t->vals[i]);
    result_version = t->ver;
  }

  if (const delta_block* block = leaf_delta(t); block != nullptr) {
    for (uint8_t j = 0; j < block->capacity; ++j) {
      const delta_slot& slot = block->slots()[j];
      const uint64_t slot_version =
          slot.version.load(std::memory_order_acquire);
      if (slot_version != 0 && slot_version <= snapshot_version &&
          slot_version >= result_version && slot.key == key) {
        result = std::make_optional(slot.value);
        result_version = slot_version;
      }
    }
  }
  return result;
}

static inline opt<uint64_t> find(cnodeptr t, uint64_t key) {
  return find(t, key, std::numeric_limits<uint64_t>::max());
}

struct visible_leaf_record {
  uint64_t key;
  uint64_t value;
  uint64_t version;
};

static inline void scan_visible_leaf(cnodeptr t, uint64_t lower_bound,
                                     bool use_lower_bound, uint64_t n,
                                     uint64_t snapshot_version,
                                     vec<kv>& ret) {
  static constexpr uint8_t MAX_VISIBLE_RECORDS = (2 * B - 1) + MAX_FAT_SLOTS;
  std::array<visible_leaf_record, MAX_VISIBLE_RECORDS> records;
  uint8_t count = 0;

  for (uint8_t i = 0; i < t->size; ++i) {
    records[count++] = {t->keys[i], t->vals[i], t->ver};
  }
  if (const delta_block* block = leaf_delta(t); block != nullptr) {
    for (uint8_t i = 0; i < block->capacity; ++i) {
      const delta_slot& slot = block->slots()[i];
      const uint64_t slot_version =
          slot.version.load(std::memory_order_acquire);
      if (slot_version != 0 && slot_version <= snapshot_version) {
        records[count++] = {slot.key, slot.value, slot_version};
      }
    }
  }

  std::sort(records.begin(), records.begin() + count,
            [](const visible_leaf_record& lhs,
               const visible_leaf_record& rhs) {
              if (lhs.key != rhs.key) {
                return lhs.key < rhs.key;
              }
              return lhs.version < rhs.version;
            });

  for (uint8_t begin = 0; begin < count && ret.size() < n;) {
    uint8_t end = begin + 1;
    while (end < count && records[end].key == records[begin].key) {
      ++end;
    }
    const visible_leaf_record& newest = records[end - 1];
    if (!use_lower_bound || newest.key >= lower_bound) {
      ret.emplace_back(newest.key, newest.value);
    }
    begin = end;
  }
}

static inline void scan_visible_append(cnodeptr t, uint64_t n,
                                       uint64_t snapshot_version,
                                       vec<kv>& ret) {
  if (t->type == LEAF) {
    scan_visible_leaf(t, 0, false, n, snapshot_version, ret);
    return;
  }
  for (uint8_t i = 0; ret.size() < n && i <= t->size; ++i) {
    scan_visible_append(t->chs[i], n, snapshot_version, ret);
  }
}

static inline void scan_visible_search(cnodeptr t, uint64_t key, uint64_t n,
                                       uint64_t snapshot_version,
                                       vec<kv>& ret) {
  if (t->type == LEAF) {
    scan_visible_leaf(t, key, true, n, snapshot_version, ret);
    return;
  }

  const uint8_t i = findch(t, key);
  scan_visible_search(t->chs[i], key, n, snapshot_version, ret);
  for (uint8_t j = i + 1; ret.size() < n && j <= t->size; ++j) {
    scan_visible_append(t->chs[j], n, snapshot_version, ret);
  }
}

static inline vec<kv> scan(cnodeptr t, uint64_t key, uint64_t n,
                           uint64_t snapshot_version) {
  vec<kv> ret;
  if (t != nullptr && n != 0) {
    scan_visible_search(t, key, n, snapshot_version, ret);
  }
  return ret;
}

static inline vec<kv> scan(cnodeptr t, uint64_t key, uint64_t n) {
  return scan(t, key, n, std::numeric_limits<uint64_t>::max());
}

}  // namespace betree
