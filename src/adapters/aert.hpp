#pragma once

#include "../config.hpp"
#include "lib/common/types.hpp"
#include "lib/trees/aert/context.hpp"
#include "lib/trees/aert/node.hpp"
#include "lib/trees/aert/build.hpp"
#include "lib/trees/aert/handlers.hpp"
#include "lib/trees/aert/query.hpp"
#include "lib/trees/aert/update_inplace.hpp"
#include "lib/trees/aert/fat_merge.hpp"

namespace aert {

struct interface {
  using operand = aert::operand;
  using context = aert::context;
  using nodeptr = aert::nodeptr;
  using cnodeptr = aert::cnodeptr;

  using concurrent_fat_replacement=fat_replacement;
  using concurrent_fat_merge=fat_merge;
  static inline constexpr bool allow_empty_root=true;
  static nodeptr concurrent_fat_target(nodeptr root,uint64_t key) { return fat_leaf_for(root,key); }
  static size_t concurrent_fat_available(nodeptr p,uint8_t slots) {
    if(!p || !slots)return 0;
    const auto& fat=((leaf*)p)->fat;size_t used=0;
    for(auto& v:fat.versions)if(fat.acquire(v))++used;
    if(auto extra=fat.acquire(fat.extra))for(unsigned i=0;i<extra->capacity;++i)if(fat.acquire(extra->records()[i].version))++used;
    return slots>=used?slots-used:0;
  }
  static bool concurrent_fat_append(nodeptr p,uint8_t slots,uint64_t version,uint64_t key,uint64_t value) {
    return p && p->pfx==key && ((leaf*)p)->fat.append(slots,version,value);
  }
  static fat_replacement concurrent_fat_prepare(nodeptr p,uint64_t version,uint64_t key,uint64_t value) {
    return prepare_fat_leaf(p,version,key,value);
  }
  static operand concurrent_fat_cow_begin(context* ctx) {
    ctx->key_ofs=0;auto op=update_cow(ctx,&ctx->root);ctx->t_cur=ctx->root;return op;
  }

  // The native entry pipe probes only after earlier structural updates commit.
  // Otherwise retain the original stage-by-stage structural traversal.
  static operand pipeline_fat_begin(context* ctx,bool can_append) {
    if(can_append)return handle_init(ctx);
    return concurrent_fat_cow_begin(ctx);
  }

  static inline constexpr operand (*handlers[16])(context*) = {
    handle_init, handle_search, handle_insert, handle_fork,
    handle_split, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr };

  static inline void checkpoint_handler(context*) { }
  static uint64_t node_size(nodeptr t) { return aert::node_size(t); }
  static void free_node(nodeptr t) { aert::free_node(t); }

  static nodeptr build(const config& cfg, uint64_t n, kv* elems) { return aert::build(n, elems); }
  static nodeptr update(nodeptr t, uint64_t key, uint64_t val) { return aert::update_inplace(t, 0, key, val); }
  static opt<uint64_t> find(cnodeptr t, uint64_t key) { return aert::find(t, key); }
  static opt<uint64_t> find(cnodeptr t, uint64_t key, uint64_t version) { return aert::find(t, key, version); }
  static vec<kv> scan(cnodeptr t, uint64_t key, uint64_t n) { return aert::scan(t, key, n); }
  static vec<kv> scan(cnodeptr t, uint64_t key, uint64_t n, uint64_t version) { return aert::scan(t, key, n, version); }
};

}
