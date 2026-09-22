#pragma once

#include <cstdint>
#include <cstring>
#include "lib/common/fat_value.hpp"

#include "mimalloc.h"
#include "mimalloc-new-delete.h"
#include "mimalloc-override.h"

namespace art {

enum NODE_TYPE { NODE4, NODE16, NODE48, NODE256, LEAF };

struct node {
  uint64_t ver;
  uint8_t type;
  uint8_t size;
  uint8_t pfx_ofs;
  uint8_t pfx_len;
  uint32_t cdf;
  uint64_t pfx;
};

struct alignas(64) leaf {
  uint64_t ver;
  uint8_t type;
  uint8_t size;
  uint8_t pfx_ofs;
  uint8_t pfx_len;
  uint32_t _;
  uint64_t pfx;
  uint64_t val;
  contrees::fat_value fat;
};

static_assert(sizeof(leaf) == 64);

struct alignas(64) node4 {
  uint64_t ver;
  uint8_t type;
  uint8_t size;
  uint8_t pfx_ofs;
  uint8_t pfx_len;
  uint32_t _;
  uint64_t pfx;
  uint8_t keys[4];
  uint32_t __;
  node* chs[4];
};

struct alignas(64) node16 {
  uint64_t ver;
  uint8_t type;
  uint8_t size;
  uint8_t pfx_ofs;
  uint8_t pfx_len;
  uint32_t _;
  uint64_t pfx;
  uint64_t __;
  uint8_t keys[16];
  uint64_t ___[2];
  node* chs[16];
};

struct alignas(64) node48 {
  uint64_t ver;
  uint8_t type;
  uint8_t size;
  uint8_t pfx_ofs;
  uint8_t pfx_len;
  uint32_t _;
  uint64_t pfx;
  uint64_t __[5];
  uint8_t slts[256];
  node* chs[48];
};

struct alignas(64) node256 {
  uint64_t ver;
  uint8_t type;
  uint8_t size;
  uint8_t pfx_ofs;
  uint8_t pfx_len;
  uint32_t _;
  uint64_t pfx;
  uint64_t __;
  uint64_t flt[4];
  node* chs[256];
};

using nodeptr = node*;
using cnodeptr = const node*;

static inline uint8_t partial_key(uint64_t key, uint8_t pos) {
  return (key >> (56-8*pos)) & 0xFF;
}

static inline uint64_t prefix_mask(uint8_t ofs, uint8_t len) {
  if (len == 0) return 0;
  return ~0UL >> (8*(8-len)) << (8*(8-len-ofs));
}

static inline uint8_t prefix_match(cnodeptr t, uint64_t key) {
  if (t->pfx_len == 0) { return 0; }
  uint64_t d = (key^t->pfx) << (8*t->pfx_ofs);
  if (d == 0) { return t->pfx_len; }
  return __builtin_clzll(d)/8;
}

static inline nodeptr new_node(uint64_t ver, uint8_t type, uint8_t pfx_ofs, uint8_t pfx_len, uint64_t key) {
  uint msize;
  uint8_t size;
  switch (type) {
    case NODE4:   msize = sizeof(node4);   size = 2;  break;
    case NODE16:  msize = sizeof(node16);  size = 4;  break;
    case NODE48:  msize = sizeof(node48);  size = 16; break;
    case NODE256: msize = sizeof(node256); size = 48; break;
    default /* LEAF */: msize = sizeof(leaf); size = 1; }
  nodeptr t = (nodeptr)aligned_alloc(64, msize);
  t->ver = ver;
  t->type = type;
  t->size = size;
  t->pfx_ofs = pfx_ofs;
  t->pfx_len = pfx_len;
  t->cdf = 0;
  t->pfx = key;
  if (t->type != LEAF) { t->pfx &= prefix_mask(pfx_ofs, pfx_len); }
  memset((uint8_t*)t+sizeof(node), 0, msize-sizeof(node));
  return t;
}

static inline nodeptr node_copy(nodeptr t_old, uint64_t ver) {
  uint msize, cpsize;
  switch (t_old->type) {
    case NODE4:   msize = sizeof(node4);   cpsize = offsetof(node4, chs);   break;
    case NODE16:  msize = sizeof(node16);  cpsize = offsetof(node16, chs);  break;
    case NODE48:  msize = sizeof(node48);  cpsize = offsetof(node48, chs);  break;
    case NODE256: msize = sizeof(node256); cpsize = offsetof(node256, chs); break;
    default /* LEAF */: msize = sizeof(leaf); cpsize = sizeof(leaf); }
  nodeptr t_new = (nodeptr)aligned_alloc(64, msize);
  if (t_old->type == LEAF) {
    memcpy(t_new, t_old, offsetof(leaf, fat));
    auto dst = (leaf*)t_new;
    dst->val = ((leaf*)t_old)->fat.read(((leaf*)t_old)->val, ver);
    dst->fat = {};
  } else {
    memcpy(((uint8_t*)t_new), ((uint8_t*)t_old), cpsize);
  }
  t_new->ver = ver;
  return t_new;
}

static inline uint64_t node_size(cnodeptr t) {
  switch (t->type) {
    case NODE4:   return sizeof(node4);
    case NODE16:  return sizeof(node16);
    case NODE48:  return sizeof(node48);
    case NODE256: return sizeof(node256);
    default /* LEAF */: return sizeof(leaf) + ((const leaf*)t)->fat.allocated_bytes();
  }
}

static inline void free_node(nodeptr t) {
  if (t->type == LEAF) ((leaf*)t)->fat.destroy();
  free(t);
}

}
