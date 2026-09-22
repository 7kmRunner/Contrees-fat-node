#pragma once

#include <cstdint>

#include "../config.hpp"
#include "lib/common/types.hpp"
#include "lib/trees/betree/context.hpp"
#include "lib/trees/betree/node.hpp"
#include "lib/trees/betree/build.hpp"
#include "lib/trees/betree/rebuild.hpp"
#include "lib/trees/betree/fat_merge.hpp"
#include "lib/trees/betree/handlers.hpp"
#include "lib/trees/betree/query.hpp"
#include "lib/trees/betree/update_inplace.hpp"

namespace betree {

struct interface {
  using operand = betree::operand;
  using context = betree::context;
  using nodeptr = betree::nodeptr;
  using cnodeptr = betree::cnodeptr;
  using concurrent_fat_replacement = fat_replacement;
  using concurrent_fat_merge = fat_merge;

  static nodeptr concurrent_fat_target(nodeptr t, uint64_t key) {
    while (t->type != LEAF) t = t->chs[findch(t, key)];
    return t;
  }
  static size_t concurrent_fat_available(nodeptr t, uint8_t slots) {
    const auto* block = leaf_delta(t);
    if (!block) return slots;
    return block->capacity == slots ? slots-block->count : 0;
  }
  static bool concurrent_fat_append(nodeptr t, uint8_t slots,
                                    uint64_t version, uint64_t key,
                                    uint64_t value) {
    return append_leaf_delta(t, slots, version, key, value);
  }
  static fat_replacement concurrent_fat_prepare(nodeptr leaf,
                                                uint64_t version,
                                                uint64_t key,
                                                uint64_t value) {
    return prepare_fat_leaf(leaf, version, key, value);
  }
  static operand concurrent_fat_cow_begin(context* ctx) {
    if (!ctx->fat_slots) ctx->fat_slots = 2;
    auto op = update_upper_cow(ctx, &ctx->root);
    ctx->t_cur = ctx->root;
    return op;
  }

  // The native entry pipe probes only after earlier structural updates commit.
  // Otherwise retain the original stage-by-stage structural traversal.
  static operand pipeline_fat_begin(context* ctx,bool can_append) {
    if(can_append)return handle_init(ctx);
    auto op=update_upper_cow(ctx,&ctx->root);ctx->t_cur=ctx->root;return op;
  }

  static inline constexpr operand (*handlers[16])(context*) = {
    handle_init, handle_upper, handle_elastic, handle_lower,
    handle_split, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr };

  static inline void checkpoint_handler(context* ctx) {
    ctx->root = rebuild(ctx->t_past, ctx->t_past->verge, ctx->sno);
  }
  static uint64_t node_size(nodeptr t) { return betree::node_size(t); }
  static void free_node(nodeptr t) { betree::free_node(t); }
  static std::pair<uint64_t, uint64_t> checkpoint_retired_stats(nodeptr t) {
    return betree::checkpoint_retired_stats(t);
  }
  static void free_checkpoint(nodeptr t) { betree::free_checkpoint(t); }

  static nodeptr build(const config& cfg, uint64_t n, kv* elems) { return betree::build(n, cfg.num_pipes-1, elems); }
  static nodeptr update(nodeptr t, uint64_t key, uint64_t val) { return betree::update_root_inplace(t, key, val); }
  static opt<uint64_t> find(cnodeptr t, uint64_t key) { return betree::find(t, key); }
  static opt<uint64_t> find(cnodeptr t, uint64_t key, uint64_t version) {
    return betree::find(t, key, version); }
  static vec<kv> scan(cnodeptr t, uint64_t key, uint64_t n) { return betree::scan(t, key, n); }
  static vec<kv> scan(cnodeptr t, uint64_t key, uint64_t n,
                      uint64_t version) {
    return betree::scan(t, key, n, version); }
};

}
