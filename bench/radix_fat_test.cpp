#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <random>
#include <thread>
#include "lib/conctrl/seqcow.hpp"
#include "src/adapters/art.hpp"
#include "src/adapters/aert.hpp"

#include "radix_walk.hpp"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)

template<class T> void run(unsigned slots) {
  fprintf(stderr,"slots=%u start\n",slots);
  using P=typename T::nodeptr;
  std::vector<kv> data{{0,0}};
  config cfg{};
  P root=T::build(cfg,data.size(),data.data());
  // Direct contexts retain all versions; reclaim every unique allocation at end.
  std::vector<P> retired;
  auto apply=[&](uint32_t v,uint64_t key,uint64_t value) {
    typename T::context c{};
    c.sno=v; c.key=key; c.val=value; c.t_past=root; c.fat_slots=slots; c.gc_enabled=true;
    c.op=T::handlers[0](&c);
    while(c.op!=conctrl::DONE) c.op=T::handlers[c.op](&c);
    c.drain_retired([&](P p){retired.push_back(p);});
    root=c.root;
  };
  if (!slots) {
    auto old=root;
    apply(1,0,1);
    CHECK(root!=old && T::node_size(root)==64);
    CHECK(T::find(old,0,0)==0 && T::find(root,0,1)==1);
    radix_bench::walk(root,[](P p){T::free_node(p);});
    for(auto p:retired) T::free_node(p);
    return;
  }
  P initial=root;
  for(unsigned v=1;v<=slots;++v) {
    apply(v,0,v);
    CHECK(root==initial);
    for(unsigned old=0;old<=v;++old) CHECK(T::find(root,0,old)==old);
    CHECK(T::scan(root,0,1,v).at(0).second==v);
    CHECK(T::node_size(root)==64+(v<=2?0:8+(slots-2)*16));
  }
  apply(slots+1,0,slots+1); CHECK(root!=initial);
  CHECK(T::find(initial,0,slots)==slots);
  std::map<uint64_t,uint64_t> model{{0,slots+1}};
  std::mt19937_64 rng(9281);
  // Every leading byte and nibble; then random forks, inserts and repeated keys.
  uint32_t v=slots+1;
  for(unsigned i=0;i<3000;++i) {
    uint64_t key=i<256 ? uint64_t(i)<<56 : (i%2 ? rng() : model.begin()->first);
    ++v; apply(v,key,v); model[key]=v;
    CHECK(T::find(root,key,v)==v);
    if(i%37==0) {
      for(auto [k,value]:model) CHECK(T::find(root,k,v)==value);
      auto scan=T::scan(root,0,model.size(),v);
      CHECK(scan.size()==model.size());
      CHECK(std::equal(scan.begin(),scan.end(),model.begin(),[](auto a,auto b){return a.first==b.first && a.second==b.second;}));
    }
  }
  CHECK(T::find(initial,0,0)==0);
  // GC safety with a reader pinned across many replacements, plus concurrent reads.
  P final_root=nullptr;
  {
    auto scheduler=std::make_unique<conctrl::seqcow<T>>(T::build(cfg,data.size(),data.data()),2,true,slots);
    scheduler->query(0,[&](auto pinned,uint64_t version) {
      auto before=T::find(pinned,0,version);
      for(unsigned i=0;i<100;++i) {auto id=scheduler->update(0,i+1); scheduler->wait_for_processing(id);}
      CHECK(T::find(pinned,0,version)==before);
    });
    std::atomic_bool stop=false;
    std::thread reader([&]{while(!stop.load()) scheduler->query(1,[&](auto p,uint64_t ver){CHECK(T::find(p,0,ver)==ver); CHECK(T::scan(p,0,1,ver).at(0).second==ver);});});
    for(unsigned i=0;i<1000;++i){auto id=scheduler->update(0,i+101);scheduler->wait_for_processing(id);}
    stop=true;reader.join();scheduler->collect_garbage();
    auto stats=scheduler->gc_statistics();
    CHECK(stats.pending_nodes==0 && stats.pending_bytes==0);
    CHECK(stats.retired_bytes==stats.reclaimed_bytes);
    scheduler->query([&](auto p,uint64_t ver){CHECK(T::find(p,0,ver)==1100);final_root=const_cast<P>(p);});
  }
  radix_bench::walk(final_root,[](P p){T::free_node(p);});
  radix_bench::walk(root,[](P p){T::free_node(p);});
  for(auto p:retired) T::free_node(p);
}
int main(){for(auto n:{0,2,4,8}){run<art::interface>(n);run<aert::interface>(n);} puts("radix fat tests passed");}
