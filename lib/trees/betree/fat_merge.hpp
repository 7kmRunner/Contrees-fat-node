#pragma once

#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

#include "rebuild.hpp"
#include "update_cow.hpp"

namespace betree {

struct fat_replacement {
  struct deleter { void operator()(nodeptr p) const { if (p) free_node(p); } };
  using owner = std::unique_ptr<node, deleter>;
  nodeptr old = nullptr;
  uint64_t route = 0, separator = 0;
  owner left, right;
};

inline fat_replacement prepare_fat_leaf(nodeptr old, uint64_t version,
                                        uint64_t key, uint64_t value) {
  context ctx{};
  ctx.sno = static_cast<uint32_t>(version);
  ctx.key = key;
  ctx.val = value;
  auto [separator, left, right] = materialize_leaf_cow(old, &ctx);
  return {old, key, separator, fat_replacement::owner(left),
          fat_replacement::owner(right)};
}

struct fat_merge {
  struct branch { nodeptr left, right; uint64_t separator; };
  std::vector<fat_replacement::owner> allocated;
  std::vector<nodeptr> retired;
  nodeptr root = nullptr;
  nodeptr t_past = nullptr;
  uint64_t retired_count = 0;
  bool checkpoint_rebuilt = false;
  bool needs_checkpoint = false;

  nodeptr internal_like(nodeptr model, uint32_t version, nodeptr* children,
                        uint64_t* keys, size_t count) {
    fat_replacement::owner p(new_internal(version, count-1));
    p->type = model->type;
    p->verge = model->verge;
    p->height = model->height;
    std::copy_n(children, count, p->chs);
    std::copy_n(keys, count-1, p->keys);
    auto raw = p.get();
    allocated.push_back(std::move(p));
    return raw;
  }

  nodeptr elastic_parent(nodeptr model, const branch& child,
                         uint32_t version) {
    nodeptr children[] = {child.left, child.right};
    uint64_t keys[] = {child.separator};
    auto p = internal_like(model, version, children, keys, 2);
    p->type = LOWER;
    p->height = model->height + 1;
    if (p->height > E) needs_checkpoint = true;
    return p;
  }

  branch merge(nodeptr old, std::vector<fat_replacement*>& changes,
               size_t begin, size_t end, uint32_t version) {
    if (begin == end) return {old, nullptr, 0};
    if (old->type == LEAF) {
      if (end != begin+1 || changes[begin]->old != old)
        throw std::logic_error("stale or duplicate BeTree leaf replacement");
      const auto& replacement = *changes[begin];
      retired.push_back(old);
      return {replacement.left.get(), replacement.right.get(),
              replacement.separator};
    }

    // A boundary has a fixed set of elastic subtrees. If one subtree root
    // splits, wrap the two halves below the boundary instead of inserting a
    // new boundary child. This preserves BeTree's checkpoint topology.
    if (old->type == BOUNDARY) {
      std::array<nodeptr, 2*B> children;
      std::copy_n(old->chs, old->size+1, children.begin());
      size_t first = begin;
      for (size_t i = 0; i <= old->size; ++i) {
        size_t last = first;
        while (last < end &&
               (i == old->size || changes[last]->route < old->keys[i])) ++last;
        auto result = merge(old->chs[i], changes, first, last, version);
        children[i] = result.right
          ? elastic_parent(old->chs[i], result, version) : result.left;
        first = last;
      }
      std::array<uint64_t, 2*B-1> keys;
      std::copy_n(old->keys, old->size, keys.begin());
      retired.push_back(old);
      return {internal_like(old, version, children.data(), keys.data(),
                            old->size+1), nullptr, 0};
    }

    std::array<nodeptr, 4*B> children;
    std::array<uint64_t, 4*B-1> keys;
    size_t count = 0, first = begin;
    for (size_t i = 0; i <= old->size; ++i) {
      size_t last = first;
      while (last < end &&
             (i == old->size || changes[last]->route < old->keys[i])) ++last;
      auto result = merge(old->chs[i], changes, first, last, version);
      if (count) keys[count-1] = old->keys[i-1];
      children[count++] = result.left;
      if (result.right) {
        keys[count-1] = result.separator;
        children[count++] = result.right;
      }
      first = last;
    }
    retired.push_back(old);
    if (count <= 2*B)
      return {internal_like(old, version, children.data(), keys.data(), count),
              nullptr, 0};
    const size_t left_count = count/2;
    auto left = internal_like(old, version, children.data(), keys.data(),
                              left_count);
    auto right = internal_like(old, version, children.data()+left_count,
                               keys.data()+left_count, count-left_count);
    return {left, right, keys[left_count-1]};
  }

  void build(nodeptr old, std::vector<fat_replacement*>& changes,
             uint32_t version) {
    std::sort(changes.begin(), changes.end(),
              [](auto lhs, auto rhs) { return lhs->route < rhs->route; });
    auto result = merge(old, changes, 0, changes.size(), version);
    root = result.left;
    if (result.right) {
      nodeptr children[] = {result.left, result.right};
      uint64_t keys[] = {result.separator};
      root = internal_like(old, version, children, keys, 2);
      if (old->type == LEAF) {
        root->type = BOUNDARY;
        root->height = 0;
      } else {
        root->height = old->height + 1;
      }
    }
    if (needs_checkpoint) {
      t_past = root;
      root = rebuild(t_past, t_past->verge, version);
      checkpoint_rebuilt = true;
    }
    retired_count = retired.size();
  }

  template<class F> void drain_retired(F&& free_one) {
    for (auto p : retired) free_one(p);
    retired.clear();
    retired_count = 0;
  }
  void clear_retired() { retired.clear(); retired_count = 0; }
  void release(std::vector<fat_replacement*>& changes) noexcept {
    for (auto& p : allocated) p.release();
    for (auto p : changes) { p->left.release(); p->right.release(); }
  }
};

}  // namespace betree
