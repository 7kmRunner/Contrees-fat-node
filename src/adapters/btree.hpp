#pragma once

#include <cstdint>

#include "../config.hpp"
#include "lib/common/types.hpp"
#include "lib/trees/btree/context.hpp"
#include "lib/trees/btree/node.hpp"
#include "lib/trees/btree/build.hpp"
#include "lib/trees/btree/handlers.hpp"
#include "lib/trees/btree/query.hpp"
#include "lib/trees/btree/fat_merge.hpp"
#include "lib/trees/btree/update_inplace.hpp"

namespace btree {

struct interface {
  using operand = btree::operand;
  using context = btree::context;
  using nodeptr = btree::nodeptr;
  using cnodeptr = btree::cnodeptr;

  static inline constexpr operand (*handlers[16])(context*) = {
    handle_init, handle_search_down, handle_search_split, handle_fat_append,
    handle_materialize, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr };

  using concurrent_fat_replacement = fat_replacement;
  using concurrent_fat_merge = fat_merge;
  static fat_replacement concurrent_fat_prepare(nodeptr leaf, uint64_t version,
                                               uint64_t key, uint64_t value) {
    return prepare_fat_leaf(leaf, version, key, value);
  }

  // Native cyclic hooks. The entry may append on a ready path or begin
  // the original staged COW traversal; worker mode reserves before dispatch.
  static operand pipeline_fat_begin(context* ctx, bool can_append) {
    return can_append ? handle_init(ctx) : handle_init_cow(ctx);
  }
  // Entry stages remain ticket-ordered. Probe only immutable, completed
  // internal nodes; never wait for a downstream stage on a cyclic thread.
  // -1: unfinished path, 0: full leaf, 1: appended without a path copy.
  static int pipeline_fat_try(context* ctx, uint64_t committed) {
    nodeptr leaf = ctx->t_past;
    while (leaf->type != LEAF) {
      if (leaf->ver > committed && !is_cow_ready(leaf)) return -1;
      leaf = leaf->chs[findch(leaf, ctx->key)];
    }
    if (!append_leaf_delta(leaf, ctx->fat_slots, ctx->sno, ctx->key, ctx->val))
      return 0;
    ctx->root = ctx->t_past;
    return 1;
  }
  static void pipeline_fat_configure(context* ctx,bool worker_fat) {
    ctx->worker_fat=worker_fat;
    ctx->leaf_pair=false;
  }
  static int pipeline_fat_task(context* ctx) {
    return ctx->op==operand::FAT_APPEND ? 1 : ctx->op==operand::MATERIALIZE ? 2 : 0;
  }
  static int pipeline_fat_reserve(context* ctx,uint64_t committed) {
    nodeptr leaf=ctx->t_past;
    while(leaf->type!=LEAF) {
      if(leaf->ver>committed && !is_cow_ready(leaf))return -1;
      leaf=leaf->chs[findch(leaf,ctx->key)];
    }
    const auto index=reserve_leaf_delta(leaf,ctx->fat_slots,ctx->sno,ctx->key);
    if(index<0)return 0;
    ctx->t_cur=leaf;
    ctx->split_idx=index;
    ctx->root=ctx->t_past;
    ctx->op=operand::FAT_APPEND;
    return 1;
  }
  // Planner and structural writer run only while append workers are quiescent.
  static operand concurrent_fat_cow_begin(context* ctx) {
    // Force materialization even for the experimental slots=0 reference. The
    // historical baseline needed this to avoid an incomplete copy_leaf. That
    // bug is now fixed for the native pipeline; retain this materialization
    // baseline so the independent controller's protocol stays comparable.
    if (!ctx->fat_slots) ctx->fat_slots = 2;
    return handle_init_cow(ctx);
  }
  static nodeptr concurrent_fat_target(nodeptr t, uint64_t key) {
    while (t->type != LEAF) t = t->chs[findch(t, key)];
    return t;
  }
  static size_t concurrent_fat_available(nodeptr t, uint8_t slots) {
    const auto* block = leaf_delta(t);
    if (!block) return slots;
    return block->capacity == slots ? slots-block->count : 0;
  }
  static bool concurrent_fat_append(nodeptr t, uint8_t slots, uint64_t version,
                                    uint64_t key, uint64_t value) {
    return append_leaf_delta(t, slots, version, key, value);
  }

  static inline void checkpoint_handler(context* ctx) { }
  static uint64_t node_size(nodeptr t) { return btree::node_size(t); }
  static void free_node(nodeptr t) { btree::free_node(t); }

  static nodeptr build(const config& cfg, uint64_t n, kv* elems) { return btree::build(n, elems); }
  static nodeptr update(nodeptr t, uint64_t key, uint64_t val) { return btree::update_root_inplace(t, key, val); }
  static opt<uint64_t> find(cnodeptr t, uint64_t key) { return btree::find(t, key); }
  static opt<uint64_t> find(cnodeptr t, uint64_t key, uint64_t version) { return btree::find(t, key, version); }
  static vec<kv> scan(cnodeptr t, uint64_t key, uint64_t n) { return btree::scan(t, key, n); }
  static vec<kv> scan(cnodeptr t, uint64_t key, uint64_t n, uint64_t version) { return btree::scan(t, key, n, version); }
};

}
