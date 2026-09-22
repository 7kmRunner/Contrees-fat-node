#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>

#include "lib/common/arrayops.hpp"
#include "context.hpp"
#include "node.hpp"

namespace btree {

struct leaf_record {
  uint64_t key;
  uint64_t value;
  uint64_t version;
};

// Prepare topology without reading any pending value. Entry/pipeline stages
// may do this even while earlier append or materialization workers are active.
static inline std::tuple<uint64_t,nodeptr,nodeptr>
prepare_leaf_cow(nodeptr t,context* ctx) {
  // Base keys are already sorted; sort only the at-most-nine overlay keys.
  std::array<uint64_t,MAX_FAT_SLOTS+1> overlay;
  unsigned added=0;
  if(auto block=leaf_delta(t))for(unsigned i=0;i<block->capacity;++i) {
    const auto version=block->slots()[i].version.load(std::memory_order_acquire)&~DELTA_PENDING;
    if(version && version<=ctx->sno)overlay[added++]=block->slots()[i].key;
  }
  overlay[added++]=ctx->key;
  std::sort(overlay.begin(),overlay.begin()+added);
  added=std::unique(overlay.begin(),overlay.begin()+added)-overlay.begin();
  std::array<uint64_t,(2*B-1)+MAX_FAT_SLOTS+1> keys;
  const unsigned count=std::set_union(t->keys,t->keys+t->size,
      overlay.begin(),overlay.begin()+added,keys.begin())-keys.begin();
  const unsigned left_size=count<=2*B-1 ? count : count/2;
  auto left=new_leaf(ctx->sno,left_size);
  std::copy_n(keys.begin(),left_size,left->keys);
  nodeptr right=nullptr;
  if(count>left_size) {
    right=new_leaf(ctx->sno,count-left_size);
    std::copy_n(keys.begin()+left_size,count-left_size,right->keys);
  }
  return {right?right->keys[0]:ctx->key,left,right};
}

// Worker-only dependency resolution. The source leaf has stopped receiving
// reservations: its replacing path was exposed before subsequent entries.
static inline void finish_leaf_cow(context* ctx) {
  auto source=ctx->t_past;
  while(source->ver && !is_cow_ready(source)) {}
  auto block=leaf_delta(source);
  if(block)for(unsigned i=0;i<block->capacity;++i) {
    while(block->slots()[i].version.load(std::memory_order_acquire)&DELTA_PENDING) {}
  }
  auto parent=ctx->t_cur;
  nodeptr left=parent ? parent->chs[ctx->split_idx] : ctx->root;
  nodeptr right=ctx->leaf_pair ? parent->chs[ctx->split_idx+1] : nullptr;
  // Both destinations form one sorted sequence. Walk the source once rather
  // than restarting a linear findval search for every destination key.
  unsigned source_index=0;
  for(auto dest:{left,right})if(dest)for(unsigned i=0;i<dest->size;++i) {
    while(source_index<source->size && source->keys[source_index]<dest->keys[i])++source_index;
    dest->vals[i]=(source_index<source->size && source->keys[source_index]==dest->keys[i])
        ? source->vals[source_index] : 0;
  }
  auto put=[&](uint64_t key,uint64_t value) {
    auto dest=right && key>=right->keys[0] ? right : left;
    dest->vals[findval(dest,key)]=value;
  };
  // Slots were reserved in ticket order, regardless of publication order.
  if(block)for(unsigned i=0;i<block->capacity;++i) {
    const auto& slot=block->slots()[i];
    const auto version=slot.version.load(std::memory_order_acquire);
    if(version && version<=ctx->sno)put(slot.key,slot.value);
  }
  put(ctx->key,ctx->val);
  publish_cow_ready(left);
  if(right)publish_cow_ready(right);
}

static inline std::tuple<uint64_t, nodeptr, nodeptr>
materialize_leaf_cow(nodeptr t, context* ctx) {
  static constexpr uint8_t MAX_RECORDS =
    (2*B-1) + MAX_FAT_SLOTS + 1;

  std::array<leaf_record, MAX_FAT_SLOTS+1> overlay;
  uint8_t added=0;
  if (const delta_block* block=leaf_delta(t); block!=nullptr) {
    for(uint8_t i=0;i<block->capacity;++i) {
      const auto& slot=block->slots()[i];
      const auto version=slot.version.load(std::memory_order_acquire);
      if(version!=0 && version<=ctx->sno)overlay[added++]={slot.key,slot.value,version};
    }
  }
  overlay[added++]={ctx->key,ctx->val,ctx->sno};
  std::sort(overlay.begin(),overlay.begin()+added,
    [](const leaf_record& lhs,const leaf_record& rhs) {
      return lhs.key!=rhs.key ? lhs.key<rhs.key : lhs.version<rhs.version;
    });
  uint8_t distinct=0;
  for(uint8_t begin=0;begin<added;) {
    uint8_t end=begin+1;
    while(end<added && overlay[end].key==overlay[begin].key)++end;
    overlay[distinct++]=overlay[end-1];
    begin=end;
  }

  // Merge the sorted base and newest-per-key overlay without re-sorting base.
  std::array<leaf_record, MAX_RECORDS> records;
  uint8_t base=0,delta=0,unique_count=0;
  while(base<t->size || delta<distinct) {
    if(delta==distinct || (base<t->size && t->keys[base]<overlay[delta].key)) {
      records[unique_count++]={t->keys[base],t->vals[base],t->ver};
      ++base;
    } else {
      auto value=overlay[delta++];
      if(base<t->size && t->keys[base]==value.key) {
        if(t->ver>value.version)value={t->keys[base],t->vals[base],t->ver};
        ++base;
      }
      records[unique_count++]=value;
    }
  }

  if (unique_count <= 2*B-1) {
    nodeptr t_new = new_leaf(ctx->sno, unique_count);
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
  if(ctx->worker_fat)return prepare_leaf_cow(t,ctx);
  if (supported_fat_slots(ctx->fat_slots)) {
    return materialize_leaf_cow(t, ctx);
  }

  uint32_t ver = ctx->sno;
  uint64_t key = ctx->key;
  uint64_t val = ctx->val;

  uint8_t i = findval(t, key);
  if (i < t->size && t->keys[i] == key) {
    nodeptr t_new = copy_leaf(ver, t);
    t_new->vals[i] = val;
    return std::make_tuple(key, t_new, nullptr); }

  if (t->size < 2*B-1) {
    nodeptr t_new = new_leaf(ver, t->size+1);
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

operand update_cow(context* ctx) {
  nodeptr t_cur = ctx->t_cur;
  uint32_t ver = ctx->sno;

  uint8_t i = findch(t_cur, ctx->key);
  nodeptr t_past = ctx->t_past = t_cur->chs[i];

  if (t_past->type == LEAF) {
    auto [key, t_l, t_r] = update_leaf_cow(t_past, ctx);
    if (t_r == nullptr) { t_cur->chs[i] = t_l; }
    else { insert_child(t_cur, key, t_l, t_r, i); }
    ctx->retire_replaced(t_past);
    publish_cow_ready(t_cur);
    if(ctx->worker_fat) {
      ctx->split_idx=i;
      ctx->leaf_pair=t_r!=nullptr;
      return operand::MATERIALIZE;
    }
    return operand::DONE; }

  if (t_past->size == 2*B-1) {
    auto [key, t_l, t_r] = split(ver, t_past);
    insert_child(t_cur, key, t_l, t_r, i);
    ctx->split_idx = i;
    ctx->retire_replaced(t_past);
    publish_cow_ready(t_cur);
    return operand::SEARCH_SPLIT; }

  t_cur->chs[i] = copy_internal(ver, t_past);
  ctx->t_cur = t_cur->chs[i];
  ctx->retire_replaced(t_past);
  publish_cow_ready(t_cur);
  return operand::SEARCH_DOWN;
}

}
