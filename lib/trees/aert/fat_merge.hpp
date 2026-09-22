#pragma once
#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <vector>
#include "node_wrapper.hpp"

namespace aert {
struct fat_replacement {
  struct deleter { void operator()(nodeptr p) const { if(p)free_node(p); } };
  using owner=std::unique_ptr<node,deleter>;
  nodeptr old=nullptr;
  uint64_t route=0;
  owner value;
};
inline fat_replacement prepare_fat_leaf(nodeptr old,uint64_t version,uint64_t key,uint64_t value) {
  return {extract_ptr(old),key,fat_replacement::owner(new_leaf(version,0,key,value))};
}

// Owners and retirement entries always hold physical pointers. Only tree edges
// carry tags; a tagged edge represents one implicit four-bit branch.
struct fat_merge {
  std::vector<fat_replacement::owner> allocated;
  std::vector<nodeptr> retired;
  nodeptr root=nullptr,t_past=nullptr;
  uint64_t retired_count=0;
  bool checkpoint_rebuilt=false;
  nodeptr own(nodeptr p) {
    fat_replacement::owner guard(p);allocated.push_back(std::move(guard));return p;
  }
  static unsigned common(uint64_t a,uint64_t b,unsigned depth) {
    if(depth==64)return 64;
    auto d=(a^b)<<depth;return d?depth+__builtin_clzll(d):64;
  }
  nodeptr clone(nodeptr old,unsigned depth,uint64_t version) {
    auto p=own(node_copy(old,version));if(old->type!=LEAF)copych(p,old);
    p->pfx_len=old->pfx_ofs+old->pfx_len-depth;p->pfx_ofs=depth;
    if(p->type!=LEAF)p->pfx&=prefix_mask(depth,p->pfx_len);
    retired.push_back(old);return p;
  }
  static void children(nodeptr p,std::array<nodeptr,256>& chs) {
    unsigned limit=node_key_len(p->type)==4?16:256;
    for(unsigned k=0;k<limit;++k)chs[k]=findch(p,k);
  }
  nodeptr assemble(std::array<nodeptr,256>& chs,unsigned depth,unsigned branch,
                   unsigned width,uint64_t key,uint64_t version) {
    unsigned count=0;for(auto p:chs)count+=p!=nullptr;
    if(width==8 && count>16) {
      unsigned highs=0;for(unsigned h=0;h<16;++h){bool any=false;for(unsigned l=0;l<16;++l)any|=chs[h*16+l]!=nullptr;highs+=any;}
      auto upper=own(new_nodeh(version,highs,depth,branch-depth,key));
      for(unsigned h=0;h<16;++h){unsigned n=0,last=0;for(unsigned l=0;l<16;++l)if(chs[h*16+l]){++n;last=l;}
        if(!n)continue;
        nodeptr child;
        if(n==1)child=embed_ptr(chs[h*16+last],0x10|last);
        else {child=own(new_nodeh(version,n,branch+4,0,0));
          for(unsigned l=0;l<16;++l)if(chs[h*16+l])insertch(child,append(child,l),chs[h*16+l]);}
        insertch(upper,append(upper,h),child);
      }
      return upper;
    }
    auto type=width==4?(count<=4?NODEH4:NODEH16):(count<=4?NODE4:NODE16);
    auto p=own(new_node(version,type,depth,branch-depth,key));p->size=0;
    for(unsigned k=0;k<(1u<<width);++k)if(chs[k])insertch(p,append(p,k),chs[k]);
    return p;
  }
  nodeptr merge(nodeptr tagged,std::vector<fat_replacement*>& changes,size_t begin,size_t end,
                unsigned depth,uint64_t version) {
    auto [old,tag]=unembed_ptr(tagged);
    if(begin==end) {
      if(!old || tag || old->pfx_ofs==depth)return tagged;
      return clone(old,depth,version);
    }
    auto firstkey=changes[begin]->route,lastkey=changes[end-1]->route;
    // Preserve or expand the implicit nibble, including newly created leaves
    // below an upper-half node. Physical leaf prefixes remain byte-aligned.
    if(tag || (!old && depth%8==4)) {
      unsigned nib=tag?(tag&15):partial_key(firstkey,depth,4);
      if(partial_key(firstkey,depth,4)==nib && partial_key(lastkey,depth,4)==nib)
        return embed_ptr(merge(old,changes,begin,end,depth+4,version),0x10|nib);
      std::array<nodeptr,256> chs{};if(old)chs[nib]=old;
      size_t pos=begin;
      for(unsigned k=0;k<16;++k){size_t next=pos;while(next<end && partial_key(changes[next]->route,depth,4)==k)++next;
        if(chs[k] || pos!=next)chs[k]=merge(chs[k],changes,pos,next,depth+4,version);pos=next;}
      return assemble(chs,depth,depth,4,firstkey,version);
    }
    unsigned branch=depth+(common(firstkey,lastkey,depth)-depth)/8*8;
    if(old)branch=std::min({branch,unsigned(old->pfx_ofs+old->pfx_len),depth+(common(firstkey,old->pfx,depth)-depth)/8*8});
    if(branch==64) {
      if(end!=begin+1 || changes[begin]->old!=old)throw std::logic_error("duplicate or stale AERT replacement");
      auto p=changes[begin]->value.get();p->pfx_ofs=depth;p->pfx_len=64-depth;
      if(old)retired.push_back(old);return p;
    }
    bool same=old && old->type!=LEAF && branch==old->pfx_ofs+old->pfx_len;
    unsigned width=same?node_key_len(old->type):8;
    bool fits=same;
    if(fits && old->type!=NODEH16){unsigned required=old->size;
      for(size_t pos=begin;pos<end;){auto k=partial_key(changes[pos]->route,branch,width);size_t next=pos+1;
        while(next<end && partial_key(changes[next]->route,branch,width)==k)++next;
        if(!findch(old,k))++required;pos=next;}
      fits=required<=unsigned(old->type==NODE4 || old->type==NODEH4?4:16);
    }
    if(fits){auto p=clone(old,depth,version);
      for(size_t pos=begin;pos<end;){auto k=partial_key(changes[pos]->route,branch,width);size_t next=pos+1;
        while(next<end && partial_key(changes[next]->route,branch,width)==k)++next;
        auto previous=findch(old,k);auto child=merge(previous,changes,pos,next,branch+width,version);
        if(previous)*getch(p,findidx(p,k)-1)=child;else insertch(p,append(p,k),child);pos=next;}
      return p;
    }
    std::array<nodeptr,256> chs{};
    if(same)children(old,chs);else if(old)chs[partial_key(old->pfx,branch,width)]=old;
    size_t pos=begin;
    for(unsigned k=0;k<(1u<<width);++k){size_t next=pos;while(next<end && partial_key(changes[next]->route,branch,width)==k)++next;
      if(chs[k] || pos!=next)chs[k]=merge(chs[k],changes,pos,next,branch+width,version);pos=next;}
    auto p=assemble(chs,depth,branch,width,firstkey,version);if(same)retired.push_back(old);return p;
  }
  void build(nodeptr old,std::vector<fat_replacement*>& changes,uint32_t version) {
    std::sort(changes.begin(),changes.end(),[](auto a,auto b){return a->route<b->route;});
    root=merge(old,changes,0,changes.size(),0,version);retired_count=retired.size();
  }
  template<class F> void drain_retired(F&& f){for(auto p:retired)f(p);retired.clear();retired_count=0;}
  void clear_retired(){retired.clear();retired_count=0;}
  void release(std::vector<fat_replacement*>& changes) noexcept {
    for(auto& p:allocated)p.release();for(auto p:changes)p->value.release();
  }
};
}
