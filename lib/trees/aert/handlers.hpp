#pragma once

#include <cstdint>

#include "node_wrapper.hpp"
#include "context.hpp"
#include "update_cow.hpp"
#include "fat_lookup.hpp"

namespace aert {

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
  nodeptr t_cur = extract_ptr(ctx->t_cur);
  copych(t_cur, ctx->t_past);
  ctx->t_past = *getch(ctx->t_past, ctx->ch_idx);
  nodeptr* t_cur_next = getch(t_cur, ctx->ch_idx);
  operand op = update_cow(ctx, t_cur_next);
  ctx->t_cur = *t_cur_next;
  std::tie(ctx->t_past, ctx->embed_pfx) = unembed_ptr(ctx->t_past);
  return op;
}

operand handle_split(context* ctx) {
  node16* t_past = (node16*)ctx->t_past;
  for (uint8_t idx = 0; idx < t_past->size; ++idx) {
    create_lower_half(ctx->t_cur, t_past->chs[idx], ctx->sno, t_past->keys[idx]); }
  nodeptr t_leaf = new_leaf(ctx->sno, ctx->key_ofs, ctx->key, ctx->val);
  uint8_t pkey = partial_key(ctx->key, ctx->key_ofs-8, 8);
  create_lower_half(ctx->t_cur, t_leaf, ctx->sno, pkey);
  return operand::DONE;
}

operand handle_insert(context* ctx) {
  nodeptr t_cur = extract_ptr(ctx->t_cur);
  copych(t_cur, ctx->t_past, ctx->ch_idx);
  nodeptr* ch = getch(t_cur, ctx->ch_idx);
  if (ctx->key_ofs % 8 == 0) {
    *ch = new_leaf(ctx->sno, ctx->key_ofs, ctx->key, ctx->val); }
  else {
    *ch = new_leaf(ctx->sno, ctx->key_ofs+4, ctx->key, ctx->val);
    *ch = embed_ptr(*ch, partial_key(ctx->key, ctx->key_ofs, 4) | 0x10); }
  return operand::DONE;
}

operand handle_fork(context* ctx) {
  ctx->t_cur = ((node4*)extract_ptr(ctx->t_cur))->chs[ctx->ch_idx];
  copych(extract_ptr(ctx->t_cur), ctx->t_past);
  return operand::DONE;
}

}
