#pragma once

#include <cstdint>

#include "context.hpp"
#include "node_wrapper.hpp"
#include "update_inplace.hpp"

namespace aert {

static inline nodeptr node_fork_cow(context* ctx, nodeptr t_past, uint8_t pmlen) {
  node4* t_par = (node4*)new_node(ctx->sno, NODE4, ctx->key_ofs, pmlen, ctx->key);

  nodeptr t_ch = node_copy(t_past, ctx->sno);
  t_ch->pfx_ofs += pmlen+8;
  t_ch->pfx_len -= pmlen+8;
  if (t_ch->type != LEAF) { t_ch->pfx &= prefix_mask(t_ch->pfx_ofs, t_ch->pfx_len); }
  uint8_t pk0 = partial_key(t_past->pfx, t_past->pfx_ofs+pmlen, 8);

  ctx->key_ofs += pmlen;
  uint8_t pk1 = partial_key(ctx->key, ctx->key_ofs, 8);

  ctx->ch_idx = pk0 > pk1;
  t_par->keys[ctx->ch_idx] = pk0;
  t_par->chs[ctx->ch_idx] = t_ch;
  t_par->keys[1-ctx->ch_idx] = pk1;
  t_par->chs[1-ctx->ch_idx] = new_leaf(ctx->sno, ctx->key_ofs+8, ctx->key, ctx->val);
  return (nodeptr)t_par;
}

static inline nodeptr nodeh_fork_cow(context* ctx) {
  nodeh4* t_par = (nodeh4*)new_node(ctx->sno, NODEH4, ctx->key_ofs, 0, ctx->key);
  uint8_t pk0 = ((uint64_t)ctx->t_past) & 0x0f;
  uint8_t pk1 = partial_key(ctx->key, ctx->key_ofs, 4);

  ctx->ch_idx = pk0 > pk1;
  t_par->keys[ctx->ch_idx] = pk0;
  t_par->chs[ctx->ch_idx] = (nodeptr)(((uint64_t)ctx->t_past) & ~0x1f);
  t_par->keys[1-ctx->ch_idx] = pk1;
  t_par->chs[1-ctx->ch_idx] = (nodeptr)new_leaf(ctx->sno, ctx->key_ofs+4, ctx->key, ctx->val);
  return (nodeptr)t_par;
}

static inline operand update_cow(context* ctx, nodeptr* tp) {
  auto [t_past, ppfx] = unembed_ptr(ctx->t_past);

  if (t_past == nullptr) {
    *tp = new_leaf(ctx->sno, ctx->key_ofs, ctx->key, ctx->val);
    return operand::DONE; }

  bool is_embed = ppfx & 0x10;
  if (is_embed && ptr_pfx_match(ppfx, t_past->pfx_ofs, ctx->key) < 4) {
    *tp = nodeh_fork_cow(ctx);
    return operand::DONE; }

  ctx->key_ofs += is_embed * 4;
  uint8_t pmlen = prefix_match(t_past, ctx->key) & 0xf8;
  if (pmlen < t_past->pfx_len) {
    *tp = node_fork_cow(ctx, t_past, pmlen);
    *tp = embed_ptr(*tp, ppfx);
    ctx->retire_replaced(t_past);
    return operand::FORK; }

  uint8_t key_len = node_key_len(t_past->type);
  if (t_past->type == LEAF) {
    *tp = leaf_update(ctx->sno, t_past, ctx->val);
    *tp = embed_ptr(*tp, ppfx);
    ctx->retire_replaced(t_past);
    return operand::DONE; }

  ctx->key_ofs += t_past->pfx_len;
  uint8_t pkey = partial_key(ctx->key, ctx->key_ofs, key_len);
  ctx->key_ofs += key_len;
  uint8_t idx = findidx(t_past, pkey);
  if (idx) {
    ctx->ch_idx = idx-1;
    *tp = node_copy(t_past, ctx->sno);
    *tp = embed_ptr(*tp, ppfx);
    ctx->retire_replaced(t_past);
    return operand::SEARCH; }

  if (t_past->size == 16) {
    *tp = create_upper_half((node16*)t_past, ctx->sno, pkey);
    *tp = embed_ptr(*tp, ppfx);
    ctx->retire_replaced(t_past);
    return operand::SPLIT; }

  if (is_full(t_past)) {
    ctx->ch_idx = extend_append(tp, t_past, ctx->sno, pkey); }
  else {
    ctx->ch_idx = copy_append(tp, t_past, ctx->sno, pkey); }

  *tp = embed_ptr(*tp, ppfx);
  ctx->retire_replaced(t_past);
  return operand::INSERT;
}

}
