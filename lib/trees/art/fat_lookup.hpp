#pragma once
#include "node_wrapper.hpp"
namespace art {
static inline nodeptr fat_leaf_for(cnodeptr t, uint64_t key) {
  while (t != nullptr) {
    uint8_t pmlen = prefix_match(t, key);
    if (t->pfx_len > pmlen) { return nullptr; }
    if (t->type == LEAF) { return (nodeptr)t; }
    uint8_t pkey = partial_key(key, t->pfx_ofs+t->pfx_len);
    t = findch(t, pkey); }
  return nullptr;
}

}
