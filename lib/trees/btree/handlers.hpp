#pragma once

#include "lib/common/arrayops.hpp"
#include "node.hpp"
#include "context.hpp"
#include "update_cow.hpp"

namespace btree {

// Structural path entry, also used by the experimental controller after all
// append workers have joined. The legacy entry below retains its fast-path gate.
operand handle_init_cow(context* ctx) {
  nodeptr t_past = ctx->t_past;
  nodeptr* tp = &ctx->root;
  if (t_past->type == LEAF) {
    auto [key, t_l, t_r] = update_leaf_cow(t_past, ctx);
    if (t_r == nullptr) { *tp = t_l; }
    else { *tp = new_root(ctx->sno, key, t_l, t_r); }
    ctx->retire_replaced(t_past);
    if(ctx->worker_fat) {
      ctx->t_cur=t_r ? *tp : nullptr;
      ctx->split_idx=0;
      ctx->leaf_pair=t_r!=nullptr;
      if(t_r)publish_cow_ready(*tp);
      return operand::MATERIALIZE;
    }
    return operand::DONE; }

  if (t_past->size < 2*B-1) {
    *tp = copy_internal(ctx->sno, t_past);
    copych(*tp, t_past);
    ctx->t_cur = *tp; }
  else {
    auto [key, t_l, t_r] = split(ctx->sno, t_past);
    *tp = new_root(ctx->sno, key, t_l, t_r);
    copych_split((*tp)->chs, t_past);
    ctx->t_cur = ctx->key < key ? (*tp)->chs[0] : (*tp)->chs[1]; }

  ctx->retire_replaced(t_past);
  auto current = ctx->t_cur;
  const auto result = update_cow(ctx);
  if (*tp != current) {
    // A root split creates an extra level in this entry stage. The sibling
    // is complete too, although only the selected child runs update_cow.
    publish_cow_ready((*tp)->chs[(*tp)->chs[0] == current ? 1 : 0]);
    publish_cow_ready(*tp);
  }
  return result;
}

operand handle_init(context* ctx) {
  nodeptr t_past = ctx->t_past;
  nodeptr* tp = &ctx->root;
  /*
   * Fat-leaf fast path: locate the leaf without copying the search path.  A
   * successful append publishes a new logical version over the same root.
   * Once that leaf's private buffer is full, the unchanged code below makes
   * one ordinary top-down path copy and update_leaf_cow materializes it.
   *
   * This is deliberately a SeqCow-only protocol: delta_block::count has one
   * writer and is not a reservation counter for concurrent writers.
   */
  if (supported_fat_slots(ctx->fat_slots)) {
    nodeptr leaf = t_past;
    while (leaf->type != LEAF) {
      leaf = leaf->chs[findch(leaf, ctx->key)];
    }
    if (append_leaf_delta(leaf, ctx->fat_slots, ctx->sno,
                          ctx->key, ctx->val)) {
      *tp = t_past;
      return operand::DONE;
    }
  }

  return handle_init_cow(ctx);
}

operand handle_search_down(context* ctx) {
  copych(ctx->t_cur, ctx->t_past);
  return update_cow(ctx);
}

operand handle_search_split(context* ctx) {
  nodeptr t_cur = ctx->t_cur;
  copych_split(t_cur->chs+ctx->split_idx, ctx->t_past);
  ctx->t_cur = t_cur->chs[ctx->split_idx + (t_cur->keys[ctx->split_idx] <= ctx->key)];
  publish_cow_ready(t_cur->chs[ctx->split_idx + (t_cur->keys[ctx->split_idx] > ctx->key)]);
  return update_cow(ctx);
}

operand handle_fat_append(context* ctx) {
  auto& slot=leaf_delta(ctx->t_cur)->slots()[ctx->split_idx];
  slot.value=ctx->val;
  slot.version.store(ctx->sno,std::memory_order_release);
  return operand::DONE;
}

operand handle_materialize(context* ctx) {
  finish_leaf_cow(ctx);
  return operand::DONE;
}

}
