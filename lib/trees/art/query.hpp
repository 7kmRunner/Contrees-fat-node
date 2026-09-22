#pragma once

#include <cstdint>

#include "lib/common/types.hpp"
#include "node_wrapper.hpp"

namespace art {

static inline opt<uint64_t> find(cnodeptr t, uint64_t key, uint64_t version = UINT64_MAX) {
  while (t != nullptr) {
    uint8_t pmlen = prefix_match(t, key);
    if (t->pfx_len > pmlen) { return std::nullopt; }
    if (t->type == LEAF) { return std::make_optional(((const leaf*)t)->fat.read(((const leaf*)t)->val, version)); }
    uint8_t pkey = partial_key(key, t->pfx_ofs+t->pfx_len);
    t = findch(t, pkey); }
  return std::nullopt;
}

void scan_append(cnodeptr t, opt<uint64_t> pkey, uint64_t n, vec<kv>& ret, uint64_t version);

void node4_scan_append(const node4 *t, opt<uint64_t> pkey, uint64_t n, vec<kv>& ret, uint64_t version) {
  uint8_t idx = 0;
  if (pkey) { idx = node4_findgt(t, pkey.value()); }

  for (; ret.size() < n && idx < t->size; idx++) {
    scan_append(t->chs[idx], std::nullopt, n, ret, version); }
}

void node16_scan_append(const node16 *t, opt<uint64_t> pkey, uint64_t n, vec<kv>& ret, uint64_t version) {
  uint8_t idx = 0;
  if (pkey) { idx = node16_findgt(t, pkey.value()); }

  for (; ret.size() < n && idx < t->size; idx++) {
    scan_append(t->chs[idx], std::nullopt, n, ret, version); }
}

void node48_scan_append(const node48 *t, opt<uint64_t> pkey, uint64_t n, vec<kv>& ret, uint64_t version) {
  uint8_t idx = 0;
  if (pkey) { if ((idx = pkey.value()+1) == 0) { return; } }

  while (ret.size() < n) {
    if (t->slts[idx] != (uint8_t)~0) { scan_append(t->chs[t->slts[idx]], std::nullopt, n, ret, version); }
    if (++idx == 0) { break; } }
}

void node256_scan_append(const node256 *t, opt<uint64_t> pkey, uint64_t n, vec<kv>& ret, uint64_t version) {
  uint8_t idx = 0;
  if (pkey) { if ((idx = pkey.value()+1) == 0) { return; } }

  while (ret.size() < n) {
    if (t->chs[idx]) { scan_append(t->chs[idx], std::nullopt, n, ret, version); }
    if (++idx == 0) { break; } }
}

void scan_append(cnodeptr t, opt<uint64_t> pkey, uint64_t n, vec<kv>& ret, uint64_t version) {
  switch (t->type) {
    case NODE4:   return node4_scan_append((const node4*)t, pkey, n, ret, version);
    case NODE16:  return node16_scan_append((const node16*)t, pkey, n, ret, version);
    case NODE48:  return node48_scan_append((const node48*)t, pkey, n, ret, version);
    case NODE256: return node256_scan_append((const node256*)t, pkey, n, ret, version);
    default: ret.emplace_back(t->pfx, ((const leaf*)t)->fat.read(((const leaf*)t)->val, version)); }
}

void scan_search(cnodeptr t, uint64_t key, uint64_t n, vec<kv>& ret, uint64_t version) {
  if (t == nullptr) { return; }
  uint8_t pmlen = prefix_match(t, key);

  if (t->pfx_len > pmlen) {
    uint8_t t_pkey = partial_key(t->pfx, t->pfx_ofs+pmlen);
    uint8_t pkey = partial_key(key, t->pfx_ofs+pmlen);
    if (t_pkey < pkey) { return; }
    scan_append(t, std::nullopt, n, ret, version);
    return; }

  if (t->type == LEAF) {
    ret.emplace_back(t->pfx, ((const leaf*)t)->fat.read(((const leaf*)t)->val, version));
    return; }

  uint8_t pkey = partial_key(key, t->pfx_ofs+t->pfx_len);
  scan_search(findch(t, pkey), key, n, ret, version);
  scan_append(t, std::make_optional(pkey), n, ret, version);
}

static inline vec<kv> scan(cnodeptr t, uint64_t key, uint64_t n, uint64_t version = UINT64_MAX) {
  vec<kv> ret;
  scan_search(t, key, n, ret, version);
  return ret;
}

}
