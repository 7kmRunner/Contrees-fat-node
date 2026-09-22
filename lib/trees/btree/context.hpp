#pragma once

#include <cstdint>

#include "lib/conctrl/context.hpp"
#include "node.hpp"

namespace btree {

enum operand {
  INIT         = conctrl::INIT,
  SEARCH_DOWN  = 0x1,
  SEARCH_SPLIT = 0x2,
  FAT_APPEND   = 0x3,
  MATERIALIZE  = 0x4,
  DONE         = conctrl::DONE
};

struct alignas(128) context : conctrl::context<node> {
  uint8_t split_idx;
  bool worker_fat=false;
  bool leaf_pair=false;
};
static_assert(sizeof(context)==128);

}
