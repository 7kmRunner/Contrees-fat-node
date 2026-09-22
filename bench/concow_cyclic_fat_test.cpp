#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include "lib/conctrl/concow_cyclic.hpp"
#include "src/adapters/btree.hpp"
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
btree::nodeptr build(unsigned n) {
  std::vector<kv> data;for(unsigned i=0;i<n;++i)data.emplace_back(i,i+1000);
  return btree::build(n,data.data());
}
void destroy(btree::nodeptr p) {
  if(p->type!=btree::LEAF)for(unsigned i=0;i<=p->size;++i)destroy(p->chs[i]);
  btree::free_node(p);
}
template<unsigned P> void capacity(unsigned slots) {
  auto root=build(1);btree::nodeptr final_root;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<btree::interface,P>>(2,root,2,true,slots);
    s->query(0,[&](auto old,uint64_t v){
      CHECK(v==0);
      for(unsigned i=1;i<=slots;++i){s->wait_for_processing(s->update(0,i));CHECK(btree::find(old,0,v)==1000);}
      CHECK(s->fat_execution_statistics().appended==slots);
      s->wait_for_processing(s->update(0,99));
      CHECK(s->fat_execution_statistics().structural==1);
      CHECK(btree::find(old,0,slots)==slots);
      CHECK(s->gc_statistics().pending_nodes==1);
      s->query(1,[&](auto p,uint64_t version){CHECK(version==slots+1);CHECK(btree::find(p,0,version)==99);final_root=const_cast<btree::nodeptr>(p);});
    });
    s->collect_garbage();CHECK(s->gc_statistics().pending_nodes==0);
  }
  destroy(final_root);
}
struct op {uint64_t ticket,key,value;};
struct observation {uint64_t version;std::vector<kv> values;};
template<class F> void await(F f) {
  auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(!f()){CHECK(std::chrono::steady_clock::now()<limit);std::this_thread::yield();}
}
struct gated : btree::interface {
  static inline std::atomic_bool entered=false,release=false,fallback=false;
  static inline unsigned searches=0;
  static operand search(context* ctx) {
    if(ctx->sno==3 && ++searches==2){entered=true;await([]{return release.load();});}
    return btree::handle_search_down(ctx);
  }
  static operand pipeline_fat_begin(context* ctx,bool ready) {
    if(ctx->sno==4){CHECK(!ready);fallback=true;}
    return btree::interface::pipeline_fat_begin(ctx,ready);
  }
  static inline constexpr operand (*handlers[16])(context*) = {
    btree::handle_init,search,btree::handle_search_split,nullptr,
    nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    nullptr,nullptr,nullptr,nullptr};
};
void fallback_dependency_test() {
  auto root=build(8192);btree::nodeptr final_root;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<gated,1>>(2,root,2,true,2);
    s->wait_for_processing(s->update(0,1));s->wait_for_processing(s->update(0,2));
    s->query(0,[&](auto old,uint64_t version){
      CHECK(version==2);
      CHECK(s->update(0,3)==3);await([]{return gated::entered.load();});
      CHECK(s->update(0,4)==4);await([]{return gated::fallback.load();});
      s->query(1,[&](auto p,uint64_t v){CHECK(v==2);CHECK(btree::find(p,0,v)==2);});
      CHECK(btree::find(old,0,version)==2);
      gated::release=true;s->wait_for_processing(4);
      CHECK(btree::find(old,0,version)==2);
    });
    s->query([&](auto p,uint64_t v){CHECK(v==4);CHECK(btree::find(p,0,v)==4);final_root=const_cast<btree::nodeptr>(p);});
    CHECK(s->fat_execution_statistics().blocked_probes>=1);
    s->collect_garbage();CHECK(s->gc_statistics().pending_bytes==0);
  }
  destroy(final_root);
}
struct disjoint_gated : btree::interface {
  static inline std::atomic_bool entered=false,release=false,probed=false;
  static inline std::atomic_int probe_result=0;
  static inline unsigned searches=0;
  static operand search(context* ctx) {
    if(ctx->sno==3 && ++searches==2){entered=true;await([]{return release.load();});}
    return btree::handle_search_down(ctx);
  }
  static int pipeline_fat_try(context* ctx,uint64_t committed) {
    const int result=btree::interface::pipeline_fat_try(ctx,committed);
    if(ctx->sno==4){probe_result=result;probed=true;}
    return result;
  }
  static operand pipeline_fat_begin(context* ctx,bool ready) {
    auto result=btree::interface::pipeline_fat_begin(ctx,ready);
    if(ctx->sno==4){CHECK(!ready);probe_result=-1;probed=true;}
    return result;
  }
  static inline constexpr operand (*handlers[16])(context*) = {
    btree::handle_init,search,btree::handle_search_split,nullptr,
    nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    nullptr,nullptr,nullptr,nullptr};
};
void disjoint_dependency_test(bool local_probe,uint64_t key) {
  disjoint_gated::entered=false;disjoint_gated::release=false;
  disjoint_gated::probed=false;disjoint_gated::searches=0;
  auto root=build(8192);btree::nodeptr final_root;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<disjoint_gated,1>>(2,root,2,true,2,local_probe);
    s->wait_for_processing(s->update(0,1));s->wait_for_processing(s->update(0,2));
    s->query(0,[&](auto old,uint64_t version){
      CHECK(version==2);
      CHECK(s->update(0,3)==3);await([]{return disjoint_gated::entered.load();});
      CHECK(s->update(key,77)==4);await([]{return disjoint_gated::probed.load();});
      CHECK(disjoint_gated::probe_result.load()==(local_probe?1:-1));
      // Even the successful independent append cannot commit across ticket 3.
      CHECK(s->progress().committed==2);
      s->query(1,[&](auto p,uint64_t v){CHECK(v==2);CHECK(btree::find(p,key,v)==key+1000);});
      CHECK(btree::find(old,key,version)==key+1000);
      disjoint_gated::release=true;s->wait_for_processing(4);
      CHECK(btree::find(old,0,version)==2);
      CHECK(btree::find(old,key,version)==key+1000);
    });
    s->query([&](auto p,uint64_t v){
      CHECK(v==4 && btree::find(p,0,v)==3 && btree::find(p,key,v)==77);
      final_root=const_cast<btree::nodeptr>(p);
    });
    s->collect_garbage();CHECK(s->gc_statistics().pending_bytes==0);
  }
  destroy(final_root);
  std::printf("paused predecessor: %s probe, independent path %s; commit still ordered\n",
              local_probe?"local":"global",local_probe?"appended":"fell back");
}
btree::nodeptr full_tree(unsigned depth,uint64_t& next) {
  if(!depth){
    auto p=btree::new_leaf(0,2*btree::B-1);
    for(unsigned i=0;i<p->size;++i){p->keys[i]=next;p->vals[i]=next+1000;next+=2;}
    return p;
  }
  auto p=btree::new_internal(0,2*btree::B-1);
  for(unsigned i=0;i<=p->size;++i){if(i)p->keys[i-1]=next;p->chs[i]=full_tree(depth-1,next);}
  return p;
}
struct parallel_gated : btree::interface {
  static inline unsigned searches[2]={0,0};
  static inline std::atomic_uint entered{0};
  static inline std::atomic_bool release{false};
  static operand search(context* ctx) {
    if(ctx->sno>=5 && ctx->sno<=6 && ++searches[ctx->sno-5]==2){
      entered.fetch_or(1U<<(ctx->sno-5));
      await([]{return release.load();});
    }
    return btree::handle_search_down(ctx);
  }
  static inline constexpr operand (*handlers[16])(context*) = {
    btree::handle_init,search,btree::handle_search_split,nullptr,
    nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    nullptr,nullptr,nullptr,nullptr};
};
void parallel_structural_test() {
  auto root=build(8192);btree::nodeptr final_root;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<parallel_gated,1>>(2,root,1,true,2);
    for(auto key:{0UL,0UL,8191UL,8191UL})s->wait_for_processing(s->update(key,1));
    CHECK(s->update(0,2)==5);CHECK(s->update(8191,2)==6);
    // Both deep handlers must enter before either is released: this cannot
    // pass if all materialization paths are executed by one coordinator.
    await([]{return parallel_gated::entered.load()==3;});
    CHECK(s->progress().committed==4);
    s->query([](auto p,uint64_t v){CHECK(v==4 && btree::find(p,0,v)==1 && btree::find(p,8191,v)==1);});
    parallel_gated::release=true;s->wait_for_processing(6);
    s->query([&](auto p,uint64_t v){CHECK(v==6 && btree::find(p,0,v)==2 && btree::find(p,8191,v)==2);final_root=const_cast<btree::nodeptr>(p);});
    s->collect_garbage();CHECK(s->gc_statistics().pending_bytes==0);
  }
  destroy(final_root);
  std::puts("two disjoint structural workers overlapped before release");
}
template<unsigned P> void split_reference(unsigned slots,bool worker_fat=false) {
  uint64_t next=0;auto root=full_tree(3,next);btree::nodeptr final_root;
  constexpr unsigned UPDATES=3000;
  std::vector<op> log(UPDATES);
  {
    auto s=std::make_unique<conctrl::concow_cyclic<btree::interface,P>>(4,root,2,true,slots,true,worker_fat);
    s->query(0,[&](auto old,uint64_t v){
      std::vector<std::thread> clients;
      for(unsigned c=0;c<4;++c)clients.emplace_back([&,c]{
        for(unsigned i=c;i<UPDATES;i+=4){
          // First fill and split nearby leaves, then race duplicate keys.
          const uint64_t key=2*(i%1000)+1,value=200000+i;
          log[i]={s->update(key,value),key,value};
        }
      });
      for(auto& t:clients)t.join();s->wait_for_processing(UPDATES);
      auto before=btree::scan(old,0,next,v);CHECK(before.size()==next/2);
      for(uint64_t i=0;i<next/2;++i)CHECK(before[i]==kv(2*i,2*i+1000));
    });
    std::map<uint64_t,uint64_t> expected;
    for(uint64_t k=0;k<next;k+=2)expected[k]=k+1000;
    std::sort(log.begin(),log.end(),[](auto a,auto b){return a.ticket<b.ticket;});
    for(auto o:log)expected[o.key]=o.value;
    s->query([&](auto p,uint64_t v){
      CHECK(v==UPDATES);auto values=btree::scan(p,0,next,v);CHECK(values.size()==expected.size());
      unsigned i=0;for(auto [k,value]:expected)CHECK(values[i++]==kv(k,value));
      final_root=const_cast<btree::nodeptr>(p);
    });
    s->collect_garbage();CHECK(s->gc_statistics().pending_bytes==0);
  }
  destroy(final_root);
  std::printf("full root/internal/leaf splits P=%u slots=%u worker_fat=%u passed\n",P,slots,worker_fat);
}
template<unsigned P> void reference(unsigned slots,unsigned workers,bool worker_fat=false) {
  constexpr unsigned N=8192,CLIENTS=4,EACH=1000;
  auto root=build(N);btree::nodeptr final_root;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<btree::interface,P>>(workers,root,2,true,slots,true,worker_fat);
    std::vector<op> log[CLIENTS];std::vector<observation> observations;
    std::atomic_bool done=false;
    s->query(0,[&](auto old,uint64_t v){
      std::thread reader([&]{while(!done.load()) {
        s->query(1,[&](auto p,uint64_t version){
          if(observations.size()<64)observations.push_back({version,btree::scan(p,0,20000,version)});
          CHECK(btree::find(p,0,version).has_value());
        });std::this_thread::yield();
      }});
      std::vector<std::thread> clients;
      for(unsigned c=0;c<CLIENTS;++c)clients.emplace_back([&,c]{
        std::mt19937_64 rng(123+c);
        for(unsigned i=0;i<EACH;++i){
          uint64_t key=i%3==0?0:(i%3==1?rng()%N:N+c*EACH+i);
          uint64_t value=100000+c*EACH+i;
          log[c].push_back({s->update(key,value),key,value});
        }
      });
      for(auto& t:clients)t.join();s->wait_for_processing(CLIENTS*EACH);done=true;reader.join();
      for(unsigned k=0;k<N;++k)CHECK(btree::find(old,k,v)==k+1000);
      s->query(1,[&](auto p,uint64_t version){observations.push_back({version,btree::scan(p,0,20000,version)});final_root=const_cast<btree::nodeptr>(p);});
    });
    std::vector<op> all;for(auto& l:log)all.insert(all.end(),l.begin(),l.end());
    std::sort(all.begin(),all.end(),[](auto a,auto b){return a.ticket<b.ticket;});
    std::map<uint64_t,uint64_t> expected;for(unsigned k=0;k<N;++k)expected[k]=k+1000;
    unsigned pos=0;
    for(auto& o:observations){
      while(pos<o.version){CHECK(all[pos].ticket==pos+1);expected[all[pos].key]=all[pos].value;++pos;}
      CHECK(o.values.size()==expected.size());unsigned j=0;
      for(auto [k,v]:expected)CHECK(o.values[j++]==kv(k,v));
    }
    s->collect_garbage();auto gc=s->gc_statistics();CHECK(gc.pending_bytes==0 && gc.retired_bytes==gc.reclaimed_bytes);
    if(slots){auto stats=s->fat_execution_statistics();CHECK(stats.appended+stats.structural==CLIENTS*EACH);CHECK(stats.structural>0);
      if(worker_fat){CHECK(stats.worker_appends==stats.appended);CHECK(stats.worker_materializations==stats.structural);}}
  }
  destroy(final_root);
  std::printf("native cyclic P=%u slots=%u workers=%u worker_fat=%u passed\n",P,slots,workers,worker_fat);std::fflush(stdout);
}
int main(){
  fallback_dependency_test();
  for(auto key:{1024UL,8191UL}){disjoint_dependency_test(false,key);disjoint_dependency_test(true,key);}
  parallel_structural_test();
  for(unsigned slots:{0,2,4,8}){split_reference<1>(slots);split_reference<3>(slots);split_reference<1>(slots,true);split_reference<3>(slots,true);}
  {
    auto old=build(1);btree::nodeptr current;
    {
      auto s=std::make_unique<conctrl::concow_cyclic<btree::interface,1>>(1,old,1,false,2);
      s->wait_for_processing(s->update(0,1));
      s->query([&](auto p,uint64_t version){
        CHECK(version==1);s->wait_for_processing(s->update(0,2));
        s->wait_for_processing(s->update(0,3));CHECK(btree::find(p,0,version)==1);
      });
      s->query([&](auto p,uint64_t v){CHECK(v==3 && btree::find(p,0,v)==3);current=const_cast<btree::nodeptr>(p);});
    }
    destroy(old);destroy(current);
  }
  for(unsigned slots:{2,4,8}){capacity<1>(slots);capacity<3>(slots);capacity<5>(slots);}
  for(unsigned slots:{0,2,4,8})for(unsigned workers:{1,2,4}){
    reference<1>(slots,workers);reference<3>(slots,workers);reference<5>(slots,workers);
    reference<1>(slots,workers,true);reference<3>(slots,workers,true);reference<5>(slots,workers,true);
  }
  std::puts("native ConCow fat tests passed");
}
