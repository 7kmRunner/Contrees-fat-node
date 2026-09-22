#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>

#include "lib/common/arrayops.hpp"
#include "context.hpp"
#include "node.hpp"

namespace betree {

struct leaf_record {
  uint64_t key;
  uint64_t value;
  uint64_t version;
};

static inline std::tuple<uint64_t, nodeptr, nodeptr>
materialize_leaf_cow(nodeptr t, context* ctx) {
  static constexpr uint8_t MAX_RECORDS =
    (2*B-1) + MAX_FAT_SLOTS + 1;

  std::array<leaf_record, MAX_RECORDS> records;
  uint8_t count = 0;

  for (uint8_t i = 0; i < t->size; ++i) {
    records[count++] = { t->keys[i], t->vals[i], t->ver };
  }

  if (const delta_block* block = leaf_delta(t); block != nullptr) {
    for (uint8_t i = 0; i < block->capacity; ++i) {
      const delta_slot& slot = block->slots()[i];
      const uint64_t slot_version =
        slot.version.load(std::memory_order_acquire);
      if (slot_version != 0 && slot_version <= ctx->sno) {
        records[count++] = { slot.key, slot.value, slot_version };
      }
    }
  }

  records[count++] = { ctx->key, ctx->val, ctx->sno };
  std::sort(records.begin(), records.begin()+count,
    [](const leaf_record& lhs, const leaf_record& rhs) {
      if (lhs.key != rhs.key) { return lhs.key < rhs.key; }
      return lhs.version < rhs.version;
    });

  /* Entries for the same key are adjacent; retain their newest value. */
  uint8_t unique_count = 0;
  for (uint8_t begin = 0; begin < count; ) {
    uint8_t end = begin + 1;
    while (end < count && records[end].key == records[begin].key) { ++end; }
    records[unique_count++] = records[end-1];
    begin = end;
  }

  if (unique_count <= 2*B-1) {
    nodeptr t_new = new_leaf(ctx->sno, unique_count);
    t_new->verge = t->verge;
    for (uint8_t i = 0; i < unique_count; ++i) {
      t_new->keys[i] = records[i].key;
      t_new->vals[i] = records[i].value;
    }
    return std::make_tuple(ctx->key, t_new, nullptr);
  }

  const uint8_t left_size = unique_count/2;
  const uint8_t right_size = unique_count-left_size;
  nodeptr t_l = new_leaf(ctx->sno, left_size);
  nodeptr t_r = new_leaf(ctx->sno, right_size);
  for (uint8_t i = 0; i < left_size; ++i) {
    t_l->keys[i] = records[i].key;
    t_l->vals[i] = records[i].value;
  }
  for (uint8_t i = 0; i < right_size; ++i) {
    t_r->keys[i] = records[left_size+i].key;
    t_r->vals[i] = records[left_size+i].value;
  }
  return std::make_tuple(t_r->keys[0], t_l, t_r);
}

static inline std::tuple<uint64_t, nodeptr, nodeptr> update_leaf_cow(nodeptr t, context* ctx) {
  if (supported_fat_slots(ctx->fat_slots)) {
    return materialize_leaf_cow(t, ctx);
  }

  uint32_t ver = ctx->sno;
  uint64_t key = ctx->key;
  uint64_t val = ctx->val;

  uint8_t i = findval(t, key);
  if (i < t->size && t->keys[i] == key) {
    nodeptr t_new = copy_leaf(t, ver);
    t_new->vals[i] = val;
    return std::make_tuple(key, t_new, nullptr); }

  if (t->size < 2*B-1) {
    nodeptr t_new = new_leaf(ver, t->size+1);
    t_new->verge = t->verge;
    copy_insert(t_new->keys, t->keys, t->size, i, key);
    copy_insert(t_new->vals, t->vals, t->size, i, val);
    return std::make_tuple(key, t_new, nullptr); }

  nodeptr t_l = new_leaf(ver, B);
  nodeptr t_r = new_leaf(ver, B);
  if (i < B) {
    copy_insert(t_l->keys, t->keys, B-1, i, key);
    copy_insert(t_l->vals, t->vals, B-1, i, val);
    copy(t_r->keys, t->keys+B-1, B);
    copy(t_r->vals, t->vals+B-1, B); }
  else {
    copy(t_l->keys, t->keys, B);
    copy(t_l->vals, t->vals, B);
    copy_insert(t_r->keys, t->keys+B, B-1, i-B, key);
    copy_insert(t_r->vals, t->vals+B, B-1, i-B, val); }
  return std::make_tuple(t_r->keys[0], t_l, t_r);
}

operand update_upper_cow(context* ctx, nodeptr* tp) {
  nodeptr t_past = ctx->t_past;
  uint32_t ver = ctx->sno;

  if (t_past->type == LEAF) {
    auto [key, t_l, t_r] = update_leaf_cow(t_past, ctx);
    if (t_r == nullptr) { *tp = t_l; }
    else {
      *tp = new_root(ver, key, t_l, t_r);
      (*tp)->verge = t_past->verge;
      (*tp)->type = BOUNDARY; }
    ctx->retire_replaced(t_past);
    return operand::DONE; }

  *tp = copy_internal(t_past, ver);
  ctx->retire_replaced(t_past);
  if ((*tp)->type == BOUNDARY) {
    ctx->wait_child = 1;
    return operand::SEARCH_ELASTIC; }
  return operand::SEARCH_UPPER;
}

operand update_lower_cow(context* ctx) {
  nodeptr t_cur = ctx->t_cur;
  uint32_t ver = ctx->sno;

  uint8_t i = findch(t_cur, ctx->key);
  nodeptr t_past = ctx->t_past = t_cur->chs[i];

  if (t_past->type == LEAF) {
    auto [key, t_l, t_r] = update_leaf_cow(t_past, ctx);
    if (t_r == nullptr) { t_cur->chs[i] = t_l; }
    else { insert_child(t_cur, key, t_l, t_r, i); }
    ctx->retire_replaced(t_past);
    return operand::DONE; }

  if (t_past->size == 2*B-1) {
    auto [key, t_l, t_r] = split(t_past, ver);
    insert_child(t_cur, key, t_l, t_r, i);
    ctx->split_idx = i;
    ctx->retire_replaced(t_past);
    return operand::SEARCH_SPLIT; }

  t_cur->chs[i] = copy_internal(t_past, ver);
  ctx->t_cur = t_cur->chs[i];
  ctx->retire_replaced(t_past);
  return operand::SEARCH_LOWER;
}

}
