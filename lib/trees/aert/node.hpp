#pragma once

#include <cstdint>
#include <cstring>
#include "lib/common/fat_value.hpp"

#include "mimalloc.h"
#include "mimalloc-new-delete.h"
#include "mimalloc-override.h"

namespace aert {

enum NODE_TYPE { NODE4, NODE16, NODEH4, NODEH16, LEAF };

static constexpr uint64_t KEY_BIT_LEN = 8*sizeof(uint64_t);

struct alignas(64) node {
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

struct alignas(64) nodeh4 {
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

struct alignas(64) nodeh16 {
  uint64_t ver;
  uint8_t type;
  uint8_t size;
  uint8_t pfx_ofs;
  uint8_t pfx_len;
  uint32_t _;
  uint64_t pfx;
  uint64_t __;
  node* chs[16];
};

using nodeptr = node*;
using cnodeptr = const node*;

static inline std::tuple<nodeptr, uint8_t> unembed_ptr(cnodeptr t) {
  uintptr_t a = (uintptr_t)t;
  return std::make_tuple((nodeptr)(a & ~0x1f), (uint8_t)(a & 0x1f));
}

template <typename T>
static inline T* extract_ptr(const T* t) {
  return (T*)((uintptr_t)t & ~0x1f);
}

template <typename T>
static inline T* embed_ptr(const T* t, uint8_t ptr_key) {
  return (T*)((uintptr_t)t | (ptr_key & 0x1f));
}

static inline uint8_t partial_key(uint64_t key, uint8_t pos, uint8_t len=8) {
  return (key >> (KEY_BIT_LEN-len-pos)) & (0xff >> (8-len));
}

static inline uint64_t prefix_mask(uint8_t ofs, uint8_t len) {
  if (len == 0) { return 0; }
  return ~0UL >> (KEY_BIT_LEN-len) << (KEY_BIT_LEN-len-ofs);
}

static inline uint8_t prefix_match(uint64_t pfx, uint8_t ofs, uint8_t len, uint64_t key) {
  if (len == 0) { return 0; }
  uint64_t d = (key^pfx) & prefix_mask(ofs, len);
  d = d << ofs;
  if (d == 0) { return len; }
  uint8_t pfx_match_len = __builtin_clzll(d);
  return pfx_match_len & 0xfc; // aligned to 4 bit
}

static inline uint8_t ptr_pfx_match(uint8_t ppfx, uint8_t ofs, uint64_t key) {
  uint64_t mask = ((uint64_t)ppfx & 0x0f) << (KEY_BIT_LEN-ofs);
  uint64_t d = ((key^mask) << (ofs-4)) | (1UL << (KEY_BIT_LEN-5));
  return __builtin_clzll(d);
}

static inline uint8_t prefix_match(cnodeptr t, uint64_t key) {
  if (t->pfx_len == 0) { return 0; }
  uint64_t d = (key^t->pfx) & prefix_mask(t->pfx_ofs, t->pfx_len);
  d = d << t->pfx_ofs;
  if (d == 0) { return t->pfx_len; }
  uint8_t pfx_match_len = __builtin_clzll(d);
  return pfx_match_len & 0xfc; // aligned to 4 bit
}

static inline bool is_lower_half(cnodeptr t) {
  return (t->pfx_ofs % 8 == 4);
}

static inline bool is_upper_half(cnodeptr t) {
  return (t->pfx_ofs + t->pfx_len) % 8 == 4;
}

static inline constexpr uint8_t node_key_len(uint8_t type) {
  switch (type) {
    case NODE4:   return 8;
    case NODE16:  return 8;
    case NODEH4:  return 4;
    case NODEH16: return 4;
    default /* LEAF */: return 4; }
}

static inline nodeptr new_node(uint64_t ver, uint8_t type, uint8_t pfx_ofs, uint8_t pfx_len, uint64_t key) {
  uint64_t msize;
  uint8_t size;
  switch (type) {
    case NODE4:   msize = sizeof(node4);   size = 2;  break;
    case NODE16:  msize = sizeof(node16);  size = 4;  break;
    case NODEH4:  msize = sizeof(nodeh4);  size = 2;  break;
    case NODEH16: msize = sizeof(nodeh16); size = 0;  break;
    default /* LEAF */: msize = sizeof(leaf); size = 1; }
  nodeptr t = (nodeptr)aligned_alloc(64, msize);
  t->ver = ver;
  t->type = type;
  t->size = size;
  t->pfx_ofs = pfx_ofs;
  t->pfx_len = pfx_len;
  memset((uint8_t*)t+offsetof(node, cdf), 0, msize-offsetof(node, cdf));
  t->pfx = key;
  if (t->type != LEAF) { t->pfx &= prefix_mask(pfx_ofs, pfx_len); }
  return t;
}

static inline nodeptr new_nodeh(uint64_t ver, uint8_t size, uint8_t pfx_ofs, uint8_t pfx_len, uint64_t key) {
  nodeptr t = new_node(ver, size>4 ? NODEH16 : NODEH4, pfx_ofs, pfx_len, key);
  t->size = 0;
  return t;
}

static inline nodeptr node_copy(nodeptr t_old, uint64_t ver) {
  uint64_t msize, cpsize;
  switch (t_old->type) {
    case NODE4:   msize = sizeof(node4);   cpsize = offsetof(node4, chs);   break;
    case NODE16:  msize = sizeof(node16);  cpsize = offsetof(node16, chs);  break;
    case NODEH4:  msize = sizeof(nodeh4);  cpsize = offsetof(nodeh4, chs);  break;
    case NODEH16: msize = sizeof(nodeh16); cpsize = offsetof(nodeh16, chs); break;
    default /* LEAF */: msize = sizeof(leaf); cpsize = sizeof(leaf); }
  nodeptr t_new = (nodeptr)aligned_alloc(64, msize);
  if (t_old->type == LEAF) {
    memcpy(t_new, t_old, offsetof(leaf, fat));
    auto dst = (leaf*)t_new;
    dst->val = ((leaf*)t_old)->fat.read(((leaf*)t_old)->val, ver);
    dst->fat = {};
  } else {
    memcpy(t_new, t_old, cpsize);
  }
  t_new->ver = ver;
  return t_new;
}

static inline uint64_t node_size(cnodeptr tagged) {
  cnodeptr t = extract_ptr(tagged);
  switch (t->type) {
    case NODE4:   return sizeof(node4);
    case NODE16:  return sizeof(node16);
    case NODEH4:  return sizeof(nodeh4);
    case NODEH16: return sizeof(nodeh16);
    default /* LEAF */: return sizeof(leaf) + ((const leaf*)t)->fat.allocated_bytes();
  }
}

static inline void free_node(void* tagged) {
  auto t = extract_ptr((nodeptr)tagged);
  if (t->type == LEAF) ((leaf*)t)->fat.destroy();
  free(t);
}

}
