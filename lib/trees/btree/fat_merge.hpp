#pragma once

#include <array>
#include <memory>
#include <stdexcept>
#include <vector>
#include "update_cow.hpp"

namespace btree {

// Private leaves stay owned by their worker result until the merged root is
// ready. A failed wave destroys them without touching published tree nodes.
struct fat_replacement {
  struct deleter { void operator()(nodeptr p) const { if(p) free_node(p); } };
  using owner = std::unique_ptr<node, deleter>;
  nodeptr old = nullptr;
  uint64_t route = 0, separator = 0;
  owner left, right;
};

inline fat_replacement prepare_fat_leaf(nodeptr old, uint64_t version,
                                        uint64_t key, uint64_t value) {
  context ctx{};
  ctx.sno = static_cast<uint32_t>(version); ctx.key = key; ctx.val = value;
  auto [separator, left, right] = materialize_leaf_cow(old, &ctx);
  return {old, key, separator, fat_replacement::owner(left), fat_replacement::owner(right)};
}

// Owns new internal nodes only; leaf ownership remains with worker results.
// Retirement is accumulated once per replaced physical node, not per update.
struct fat_merge {
  struct branch { nodeptr left, right; uint64_t separator; };
  std::vector<fat_replacement::owner> allocated;
  std::vector<nodeptr> retired;
  nodeptr root = nullptr;
  uint64_t retired_count = 0;
  bool checkpoint_rebuilt = false;
  nodeptr t_past = nullptr;

  nodeptr internal(uint32_t version, nodeptr* children, uint64_t* keys, size_t count) {
    fat_replacement::owner p(new_internal(version, count-1));
    std::copy_n(children, count, p->chs);
    std::copy_n(keys, count-1, p->keys);
    auto raw = p.get(); allocated.push_back(std::move(p)); return raw;
  }

  branch merge(nodeptr old, std::vector<fat_replacement*>& changes,
               size_t begin, size_t end, uint32_t version) {
    if(begin==end) return {old, nullptr, 0};
    if(old->type==LEAF) {
      if(end!=begin+1 || changes[begin]->old!=old)
        throw std::logic_error("stale or duplicate leaf replacement");
      const auto& r=*changes[begin]; retired.push_back(old);
      return {r.left.get(),r.right.get(),r.separator};
    }
    // Each child can grow into at most two nodes in this bounded wave.
    std::array<nodeptr,4*B> children;
    std::array<uint64_t,4*B-1> keys;
    size_t count=0, first=begin;
    for(size_t i=0;i<=old->size;++i) {
      size_t last=first;
      while(last<end && (i==old->size || changes[last]->route<old->keys[i])) ++last;
      auto result=merge(old->chs[i],changes,first,last,version);
      if(count) keys[count-1]=old->keys[i-1];
      children[count++]=result.left;
      if(result.right) { keys[count-1]=result.separator; children[count++]=result.right; }
      first=last;
    }
    retired.push_back(old);
    if(count<=2*B) return {internal(version,children.data(),keys.data(),count),nullptr,0};
    const size_t left_count=count/2;
    auto left=internal(version,children.data(),keys.data(),left_count);
    auto right=internal(version,children.data()+left_count,keys.data()+left_count,count-left_count);
    return {left,right,keys[left_count-1]};
  }

  void build(nodeptr old, std::vector<fat_replacement*>& changes, uint32_t version) {
    std::sort(changes.begin(),changes.end(),[](auto a,auto b){return a->route<b->route;});
    auto result=merge(old,changes,0,changes.size(),version);
    root=result.left;
    if(result.right) {
      nodeptr children[]={result.left,result.right};
      uint64_t keys[]={result.separator};
      root=internal(version,children,keys,2);
    }
    retired_count=retired.size();
  }
  template<class F> void drain_retired(F&& f) {
    for(auto p:retired) f(p);
    retired.clear(); retired_count=0;
  }
  void clear_retired() { retired.clear(); retired_count=0; }
  void release(std::vector<fat_replacement*>& changes) noexcept {
    for(auto& p:allocated) p.release();
    for(auto p:changes) { p->left.release(); p->right.release(); }
  }
};
} // namespace btree
