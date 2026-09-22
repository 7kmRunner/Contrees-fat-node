#pragma once
#include "node_wrapper.hpp"
namespace aert {
static inline nodeptr fat_leaf_for(cnodeptr t, uint64_t key) {
  while (t != nullptr) {
    uint8_t ppfx;
    std::tie(t, ppfx) = unembed_ptr(t);
    if ((ppfx & 0x10) && ptr_pfx_match(ppfx, t->pfx_ofs, key) < 4) { return nullptr; }
    uint8_t pmlen = prefix_match(t, key) & 0xf8;
    if (t->pfx_len > pmlen) { return nullptr; }
    if (t->type == NODE_TYPE::LEAF) { return (nodeptr)t; }
    uint8_t pkey = partial_key(key, t->pfx_ofs+t->pfx_len, node_key_len(t->type));
    t = findch(t, pkey); }
  return nullptr;
}

}
