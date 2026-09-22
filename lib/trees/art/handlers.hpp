#pragma once

#include "node_wrapper.hpp"
#include "context.hpp"
#include "update_cow.hpp"
#include "fat_lookup.hpp"

namespace art {

operand handle_init(context* ctx) {
  if (ctx->fat_slots) {
    auto t = fat_leaf_for(ctx->t_past, ctx->key);
    if (t && ((leaf*)t)->fat.append(ctx->fat_slots, ctx->sno, ctx->val)) {
      ctx->root = ctx->t_past;
      return operand::DONE;
    }
  }
  ctx->key_ofs = 0;
  operand op = update_cow(ctx, &ctx->root);
  ctx->t_cur = ctx->root;
  return op;
}

operand handle_search(context* ctx) {
  copych(ctx->t_cur, ctx->t_past);
  ctx->t_past = *getch(ctx->t_past, ctx->ch_idx);
  nodeptr* t_cur_next = getch(ctx->t_cur, ctx->ch_idx);
  operand op = update_cow(ctx, t_cur_next);
  ctx->t_cur = *t_cur_next;
  return op;
}

operand handle_insert(context* ctx) {
  copych(ctx->t_cur, ctx->t_past, ctx->ch_idx);
  nodeptr* ch = getch(ctx->t_cur, ctx->ch_idx);
  *ch = new_leaf(ctx->sno, ctx->key_ofs, ctx->key, ctx->val);
  return operand::DONE;
}

operand handle_fork(context* ctx) {
  ctx->t_cur = ((node4*)ctx->t_cur)->chs[ctx->ch_idx];
  copych(ctx->t_cur, ctx->t_past);
  return operand::DONE;
}

}
