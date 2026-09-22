#pragma once

#include <cstdint>

#include "node.hpp"

namespace art {

static inline nodeptr new_leaf(uint64_t ver, uint8_t pfx_ofs, uint64_t key, uint64_t val) {
  nodeptr t = new_node(ver, LEAF, pfx_ofs, 8-pfx_ofs, key);
  ((leaf*)t)->val = val;
  return t;
}

static inline nodeptr leaf_update(uint64_t ver, nodeptr t_old, uint64_t val) {
  nodeptr t_new = node_copy(t_old, ver);
  ((leaf*)t_new)->val = val;
  return t_new;
}

void print_leaf(const leaf* t) {
#ifndef NDEBUG
  fprintf(stderr, "LEAF %lx %llx\n", (uintptr_t)t, static_cast<unsigned long long>(t->pfx));
#endif
}

}
