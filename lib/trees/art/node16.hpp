#pragma once

#include <cstdint>
#include <cstring>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

#include "lib/common/arrayops.hpp"
#include "lib/common/types.hpp"
#include "node.hpp"

namespace art {

static inline opt<uint8_t> node16_find(const node16* t, uint8_t pkey) {
#if defined(__x86_64__) || defined(__i386__)
  uint bitfield =
    _mm_movemask_epi8(
      _mm_cmpeq_epi8(
        _mm_load_si128((__m128i *)((node16*)t)->keys),
        _mm_set1_epi8(pkey))) &
    (~(~0U << t->size));
  if (!bitfield) { return std::nullopt; }
  return std::make_optional(__builtin_ctz(bitfield));
#else
  for (uint8_t i=0; i<t->size; ++i) if (t->keys[i] == pkey) { return std::make_optional(i); }
  return std::nullopt;
#endif
}

static inline uint8_t node16_findgt(const node16* t, uint8_t pkey) {
#if defined(__AVX512BW__) && defined(__AVX512VL__)
  uint mask =
    _mm_cmp_epu8_mask(
      _mm_load_si128((__m128i *)((node16*)t)->keys),
      _mm_set1_epi8(pkey),
      _MM_CMPINT_GT) &
    (~(~0U << t->size));
  if (!mask) { return t->size; }
  return __builtin_ctz(mask);
#else
  for (uint8_t i=0; i<t->size; ++i) if (t->keys[i] > pkey) return i;
  return t->size;
#endif
}

static inline nodeptr* node16_getch(node16* t, uint8_t idx) {
  return &t->chs[idx];
}

static inline nodeptr node16_findch(const node16* t, uint8_t pkey) {
  opt<uint8_t> idx = node16_find(t, pkey);
  if (!idx) { return nullptr; }
  return t->chs[idx.value()];
}

static inline uint8_t node16_append(node16* t, uint8_t pkey) {
  uint8_t idx = node16_findgt(t, pkey);
  insert(t->keys, t->size, idx, pkey);
  t->size++;
  return idx;
}

static inline void node16_insertch(node16* t, uint8_t idx, nodeptr ch) {
  insert(t->chs, t->size-1, idx, ch);
}

static inline node48* node16_extend(node16* t_old, uint64_t ver) {
  node48* t_new = (node48*)new_node(ver, NODE48, t_old->pfx_ofs, t_old->pfx_len, t_old->pfx);
  memset(t_new->slts, ~0, 256);
  for (uint8_t idx = 0; idx < 16; idx++) { t_new->slts[t_old->keys[idx]] = idx; }
  return t_new;
}

static inline void node16_copych(node16* t_new, node16* t_old) {
  copy(t_new->chs, t_old->chs, t_old->size);
}

static inline void node16_copych(node16* t_new, node16* t_old, uint8_t pos) {
  copy_insert<nodeptr>(t_new->chs, t_old->chs, t_old->size, pos, nullptr);
}

static inline void node16_copych_extend(node48* t_new, node16* t_old) {
  copy(t_new->chs, t_old->chs, t_old->size);
}

void print_node16(const node16* t) {
#ifndef NDEBUG
  fprintf(stderr, "NODE16 %lx %u\n", (uintptr_t)t, t->size);
  for (uint i = 0; i < t->size; i++) {
    fprintf(stderr, "  %x %lx\n", t->keys[i], (uintptr_t)t->chs[i]); }
#endif
}

}
