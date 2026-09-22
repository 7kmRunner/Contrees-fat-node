#pragma once

#include <cstdint>

#include "lib/common/types.hpp"
#include "context.hpp"
#include "node_wrapper.hpp"

namespace art {

static inline nodeptr node_fork_cow(context* ctx, nodeptr t_past, uint8_t pmlen) {
  node4* t_par = (node4*)new_node(ctx->sno, NODE4, ctx->key_ofs, pmlen, ctx->key);

  nodeptr t_ch = node_copy(t_past, ctx->sno);
  t_ch->pfx_ofs += pmlen+1;
  t_ch->pfx_len -= pmlen+1;
  if (t_ch->type != LEAF) { t_ch->pfx &= prefix_mask(t_ch->pfx_ofs, t_ch->pfx_len); }
  uint8_t pk0 = partial_key(t_past->pfx, t_past->pfx_ofs+pmlen);

  ctx->key_ofs += pmlen;
  uint8_t pk1 = partial_key(ctx->key, ctx->key_ofs);

  ctx->ch_idx = pk0 > pk1;
  t_par->keys[ctx->ch_idx] = pk0;
  t_par->chs[ctx->ch_idx] = t_ch;
  t_par->keys[1-ctx->ch_idx] = pk1;
  t_par->chs[1-ctx->ch_idx] = new_leaf(ctx->sno, ctx->key_ofs+1, ctx->key, ctx->val);
  return (nodeptr)t_par;
}

static inline operand update_cow(context* ctx, nodeptr* tp) {
  nodeptr t_past = ctx->t_past;

  if (t_past == nullptr) {
    *tp = new_leaf(ctx->sno, ctx->key_ofs, ctx->key, ctx->val);
    return operand::DONE; }

  uint8_t pmlen = prefix_match(t_past, ctx->key);
  if (pmlen < t_past->pfx_len) {
    *tp = node_fork_cow(ctx, t_past, pmlen);
    ctx->retire_replaced(t_past);
    return operand::FORK; }

  if (t_past->type == LEAF) {
    *tp = leaf_update(ctx->sno, t_past, ctx->val);
    ctx->retire_replaced(t_past);
    return operand::DONE; }

  ctx->key_ofs += t_past->pfx_len;
  uint8_t pkey = partial_key(ctx->key, ctx->key_ofs);
  ctx->key_ofs++;
  opt<uint8_t> idx = find_idx(t_past, pkey);
  if (idx) {
    ctx->ch_idx = idx.value();
    *tp = node_copy(t_past, ctx->sno);
    ctx->retire_replaced(t_past);
    return operand::SEARCH; }

  *tp = is_full(t_past) ? node_extend(t_past, ctx->sno) : node_copy(t_past, ctx->sno);
  ctx->ch_idx = append(*tp, pkey);
  ctx->retire_replaced(t_past);
  return operand::INSERT;
}

}
