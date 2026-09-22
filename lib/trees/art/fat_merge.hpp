#pragma once
#include <array>
#include <memory>
#include <stdexcept>
#include <vector>
#include "node_wrapper.hpp"

namespace art {
struct fat_replacement {
  struct deleter { void operator()(nodeptr p) const { if(p) free_node(p); } };
  using owner=std::unique_ptr<node,deleter>;
  nodeptr old=nullptr;
  uint64_t route=0;
  owner value;
};

inline fat_replacement prepare_fat_leaf(nodeptr old,uint64_t version,uint64_t key,uint64_t value) {
  // One key per ART leaf: the latest update replaces its entire value history.
  // Prefix offset is assigned only once, when this private leaf is installed.
  return {old,key,fat_replacement::owner(new_leaf(version,0,key,value))};
}

struct fat_merge {
  std::vector<fat_replacement::owner> allocated;
  std::vector<nodeptr> retired;
  nodeptr root=nullptr,t_past=nullptr;
  uint64_t retired_count=0;
  bool checkpoint_rebuilt=false;

  static unsigned common(uint64_t a,uint64_t b,unsigned depth) {
    if(depth==8)return 8;
    uint64_t d=(a^b)<<(8*depth);
    return d?depth+__builtin_clzll(d)/8:8;
  }
  nodeptr own(nodeptr p) {
    fat_replacement::owner guard(p);allocated.push_back(std::move(guard));return p;
  }
  nodeptr clone(nodeptr old,unsigned depth,uint64_t version) {
    auto p=own(node_copy(old,version));
    if(old->type!=LEAF)copych(p,old);
    p->pfx_len=old->pfx_ofs+old->pfx_len-depth;p->pfx_ofs=depth;
    if(p->type!=LEAF)p->pfx&=prefix_mask(depth,p->pfx_len);
    retired.push_back(old);return p;
  }
  static void children(nodeptr old,std::array<nodeptr,256>& chs) {
    switch(old->type) {
      case NODE4: {auto p=(node4*)old;for(unsigned i=0;i<p->size;++i)chs[p->keys[i]]=p->chs[i];break;}
      case NODE16: {auto p=(node16*)old;for(unsigned i=0;i<p->size;++i)chs[p->keys[i]]=p->chs[i];break;}
      case NODE48: {auto p=(node48*)old;for(unsigned i=0;i<256;++i)if(p->slts[i]!=255)chs[i]=p->chs[p->slts[i]];break;}
      case NODE256: std::copy_n(((node256*)old)->chs,256,chs.begin());break;
      default: throw std::logic_error("leaf has no radix children");
    }
  }

  nodeptr merge(nodeptr old,std::vector<fat_replacement*>& changes,size_t begin,size_t end,
                unsigned depth,uint64_t version) {
    if(begin==end)return old && old->pfx_ofs!=depth ? clone(old,depth,version) : old;
    unsigned branch=common(changes[begin]->route,changes[end-1]->route,depth);
    if(old)branch=std::min<unsigned>({branch,unsigned(old->pfx_ofs+old->pfx_len),common(changes[begin]->route,old->pfx,depth)});
    if(branch==8) {
      if(end!=begin+1 || changes[begin]->old!=old)
        throw std::logic_error("duplicate or stale ART replacement");
      auto p=changes[begin]->value.get();p->pfx_ofs=depth;p->pfx_len=8-depth;
      if(old)retired.push_back(old);
      return p;
    }
    const bool same_branch=old && old->type!=LEAF && branch==old->pfx_ofs+old->pfx_len;
    // Reuse the current node shape for updates AND insertions that fit. Avoid
    // rebuilding all 256 branches for a single missing child of Node256.
    bool fits=same_branch;
    if(fits && old->type!=NODE256) {
      unsigned required=old->size;
      for(size_t first=begin;first<end;) {
        auto key=partial_key(changes[first]->route,branch);size_t last=first+1;
        while(last<end && partial_key(changes[last]->route,branch)==key)++last;
        if(!findch(old,key))++required;
        first=last;
      }
      unsigned capacity=old->type==NODE4?4:old->type==NODE16?16:48;
      fits=required<=capacity;
    }
    if(fits) {
      auto p=clone(old,depth,version);
      for(size_t first=begin;first<end;) {
        auto key=partial_key(changes[first]->route,branch);size_t last=first+1;
        while(last<end && partial_key(changes[last]->route,branch)==key)++last;
        auto previous=findch(old,key);
        auto child=merge(previous,changes,first,last,branch+1,version);
        if(previous)*getch(p,find_idx(p,key).value())=child;
        else insertch(p,append(p,key),child);
        first=last;
      }
      return p;
    }
    std::array<nodeptr,256> chs{};
    if(same_branch)children(old,chs);
    else if(old)chs[partial_key(old->pfx,branch)]=old;
    size_t first=begin;
    for(unsigned key=0;key<256;++key) {
      size_t last=first;
      while(last<end && partial_key(changes[last]->route,branch)==key)++last;
      if(chs[key] || first!=last)chs[key]=merge(chs[key],changes,first,last,branch+1,version);
      first=last;
    }
    unsigned count=0;for(auto p:chs)if(p)++count;
    if(count<2)throw std::logic_error("invalid ART branch");
    const auto type=count<=4?NODE4:count<=16?NODE16:count<=48?NODE48:NODE256;
    auto p=own(new_node(version,type,depth,branch-depth,changes[begin]->route));
    p->size=0;
    if(type==NODE48)std::fill_n(((node48*)p)->slts,256,uint8_t(255));
    for(unsigned key=0;key<256;++key)if(chs[key])insertch(p,append(p,key),chs[key]);
    if(same_branch)retired.push_back(old);
    return p;
  }
  void build(nodeptr old,std::vector<fat_replacement*>& changes,uint32_t version) {
    std::sort(changes.begin(),changes.end(),[](auto a,auto b){return a->route<b->route;});
    root=merge(old,changes,0,changes.size(),0,version);retired_count=retired.size();
  }
  template<class F> void drain_retired(F&& f) {for(auto p:retired)f(p);retired.clear();retired_count=0;}
  void clear_retired(){retired.clear();retired_count=0;}
  void release(std::vector<fat_replacement*>& changes) noexcept {
    for(auto& p:allocated)p.release();for(auto p:changes)p->value.release();
  }
};
}
