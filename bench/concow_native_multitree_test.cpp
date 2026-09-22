#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include "lib/conctrl/concow.hpp"
#ifdef TREE_BETREE
#include "src/adapters/betree.hpp"
namespace tree=betree;
#else
#include "radix_walk.hpp"
#ifdef TREE_ART
namespace tree=art;
#else
namespace tree=aert;
#endif
#endif
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
template<class F> void await(F f){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(!f()){CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}}
struct observed:tree::interface {
  static inline thread_local bool monitor=false;
  static inline std::atomic_bool wrong_owner=false;
  static inline std::atomic_bool pause_worker=false,entered=false,release=false,fallback=false,pause_gc=false,gc_entered=false,gc_release=false;
  static inline uint64_t pause_ticket=1;
  static void before_worker_step(context* ctx){if(ctx->sno==pause_ticket && pause_worker.exchange(false)){entered=true;await([]{return release.load();});}}
  static operand pipeline_fat_begin(context* ctx,bool ready){if(ctx->sno==pause_ticket+1 && entered){CHECK(!ready);fallback=true;}return tree::interface::pipeline_fat_begin(ctx,ready);}
  static void on_gc_requested(uint64_t){if(pause_gc.exchange(false)){gc_entered=true;await([]{return gc_release.load();});}}
  static void on_monitor_pass(){monitor=true;}
#ifdef TREE_BETREE
  static void free_checkpoint(nodeptr p){if(!monitor)wrong_owner=true;tree::free_checkpoint(p);}
#endif
  static void free_node(nodeptr p){if(!monitor)wrong_owner=true;tree::free_node(p);}
};
using scheduler=conctrl::concow<observed>;
void destroy(tree::nodeptr p){
#ifdef TREE_BETREE
  if(p->type!=tree::LEAF)for(unsigned i=0;i<=p->size;++i)destroy(p->chs[i]);
  tree::free_node(p);
#else
  radix_bench::walk(p,[](auto n){tree::free_node(n);});
#endif
}
tree::nodeptr build(std::vector<kv>& data,unsigned pipes){config cfg;cfg.num_pipes=pipes;return tree::interface::build(cfg,data.size(),data.data());}
struct operation{uint64_t ticket,key,value;};
struct snapshot{uint64_t version;std::vector<kv> values;};
void capacity(unsigned slots){
  std::vector<kv> data{{0,1000}};auto initial=build(data,1);tree::nodeptr last;
  {
    auto s=std::make_unique<scheduler>(1,2,initial,1,true,slots);
    s->query([&](auto p,uint64_t v){
      for(unsigned i=1;i<=slots;++i)s->wait_for_processing(s->update(0,i));
      CHECK(s->fat_execution_statistics().appended==slots);
      s->wait_for_processing(s->update(0,99));
      CHECK(tree::find(p,0,v)==1000 && tree::find(p,0,slots)==slots);
      CHECK(s->gc_statistics().pending_nodes>0);
    });
    s->collect_garbage();CHECK(s->gc_statistics().pending_bytes==0);
    s->query([&](auto p,uint64_t v){CHECK(tree::find(p,0,v)==99);last=const_cast<tree::nodeptr>(p);});
  }destroy(last);
}
void reference(unsigned slots,unsigned pipes,unsigned workers){
  observed::wrong_owner=false;
  std::vector<kv> data;for(uint64_t i=0;i<256;++i)data.emplace_back(i<<56,i+1000);
  auto initial=build(data,pipes);tree::nodeptr last;
  {
    auto s=std::make_unique<scheduler>(pipes,workers,initial,2,true,slots);
    std::vector<operation> log[4];std::vector<snapshot> snaps;std::atomic_bool done=false;
    s->query(0,[&](auto old,uint64_t v){
      std::thread reader([&]{while(!done){s->query(1,[&](auto p,uint64_t version){if(snaps.size()<32)snaps.push_back({version,tree::scan(p,0,10000,version)});});std::this_thread::yield();}});
      std::vector<std::thread> threads;
      for(unsigned c=0;c<2;++c)threads.emplace_back([&]{for(unsigned i=0;i<100;++i)s->collect_garbage();});
      for(unsigned c=0;c<4;++c)threads.emplace_back([&,c]{std::mt19937_64 rng(182+c);
        for(unsigned i=0;i<600;++i){uint64_t key=i%3==0?0:i%3==1?(rng()%256)<<56:rng();uint64_t val=10000+c*600+i;
          log[c].push_back({s->update(key,val),key,val});}});
      for(auto& t:threads)t.join();
      uint64_t ticket=0;for(auto& l:log)for(auto o:l)ticket=std::max(ticket,o.ticket);
      s->wait_for_processing(ticket);done=true;reader.join();
      CHECK(tree::scan(old,0,10000,v)==data);
      s->query(1,[&](auto p,uint64_t version){snaps.push_back({version,tree::scan(p,0,10000,version)});last=const_cast<tree::nodeptr>(p);});
    });
    std::vector<operation> ops;for(auto& l:log)ops.insert(ops.end(),l.begin(),l.end());
    std::sort(ops.begin(),ops.end(),[](auto a,auto b){return a.ticket<b.ticket;});
    std::map<uint64_t,uint64_t> expected(data.begin(),data.end());unsigned index=0;
    for(auto& snap:snaps){while(index<ops.size() && ops[index].ticket<=snap.version){auto op=ops[index++];expected[op.key]=op.value;}
      CHECK(snap.values.size()==expected.size());unsigned i=0;for(auto [k,v]:expected)CHECK(snap.values[i++]==kv(k,v));}
    CHECK(index==ops.size());
    s->collect_garbage();auto gc=s->gc_statistics();CHECK(gc.pending_bytes==0 && gc.retired_bytes==gc.reclaimed_bytes);
    CHECK(!observed::wrong_owner.load());
    if(slots){auto stats=s->fat_execution_statistics();CHECK(stats.appended+stats.structural==2400);}
  }destroy(last);
  std::printf("native reference slots=%u pipes=%u workers=%u passed\n",slots,pipes,workers);std::fflush(stdout);
}
void paused_predecessor(){
  std::vector<kv> data;for(uint64_t i=0;i<256;++i)data.emplace_back(i<<56,i+1000);
  auto root=build(data,1);tree::nodeptr last;
  {
    auto s=std::make_unique<scheduler>(1,2,root,1,true,2);
    observed::pause_ticket=1;
#ifdef TREE_BETREE
    s->wait_for_processing(s->update(0,1000));s->wait_for_processing(s->update(0,1000));
    observed::pause_ticket=3;
#endif
    const auto prior=observed::pause_ticket-1;
    observed::pause_worker=true;observed::entered=false;observed::release=false;observed::fallback=false;
    s->update(1234567,99);await([]{return observed::entered.load();});
    s->update(0,88);await([]{return observed::fallback.load();});
    CHECK(s->committed_version()==prior);
    s->query([&](auto p,uint64_t v){CHECK(v==prior && tree::scan(p,0,10000,v)==data);});
    observed::release=true;s->wait_for_processing(observed::pause_ticket+1);observed::entered=false;
    s->query([&](auto p,uint64_t v){CHECK(tree::find(p,0,v)==88 && tree::find(p,1234567,v)==99);last=const_cast<tree::nodeptr>(p);});
    observed::pause_gc=true;observed::gc_entered=false;observed::gc_release=false;
    std::thread collector([&]{s->collect_garbage();});await([]{return observed::gc_entered.load();});
    s->wait_for_processing(s->update(0,77));s->collect_garbage();
    observed::gc_release=true;collector.join();
    s->query([&](auto p,uint64_t v){CHECK(tree::find(p,0,v)==77);last=const_cast<tree::nodeptr>(p);});
  }destroy(last);std::puts("paused structural predecessor and paused GC caller passed");
}
#ifndef TREE_BETREE
void growth(){
  tree::nodeptr last;
  {
    auto s=std::make_unique<scheduler>(1,2,nullptr,2,true,4);
    for(uint64_t i=0;i<256;++i){s->wait_for_processing(s->update(i<<56,i+1));s->wait_for_processing(s->update(i<<56,i+2));}
    s->query(0,[&](auto old,uint64_t v){
      auto before=tree::scan(old,0,10000,v);
      for(uint64_t i=0;i<256;++i)s->wait_for_processing(s->update((i<<56)|0x12345,999));
      CHECK(tree::scan(old,0,10000,v)==before);
    });
    s->query([&](auto p,uint64_t v){for(uint64_t i=0;i<256;++i){CHECK(tree::find(p,i<<56,v)==i+2);CHECK(tree::find(p,(i<<56)|0x12345,v)==999);}last=const_cast<tree::nodeptr>(p);});
    s->collect_garbage();CHECK(s->gc_statistics().pending_nodes==0);
  }destroy(last);std::puts("empty root, growth and fat-value prefix fork passed");
}
#else
void checkpoint(bool full){
  auto root=tree::new_internal(0,0);root->type=tree::BOUNDARY;root->verge=full?3:0;
  auto lower=tree::new_internal(0,15);lower->type=tree::LOWER;lower->height=tree::E;root->chs[0]=lower;
  for(unsigned i=0;i<16;++i){
    if(i)lower->keys[i-1]=i*100;
    auto leaf=tree::new_leaf(0,15);lower->chs[i]=leaf;
    for(unsigned k=0;k<15;++k){leaf->keys[k]=i*100+4*k;leaf->vals[k]=7;}
    for(unsigned k=0;k<8;++k)CHECK(tree::append_leaf_delta(leaf,8,1,i*100+4*k+1,8));
  }
  auto untouched=lower->chs[15];tree::nodeptr last;
  {
    auto s=std::make_unique<scheduler>(1,2,root,1,true,8);
    s->query([&](auto old,uint64_t v){
      s->wait_for_processing(s->update(33,9));
      uint64_t ticket=0;for(unsigned i=0;i<conctrl::BUFFER_SIZE;++i)ticket=s->update(33,99);
      s->wait_for_processing(ticket);
      CHECK(s->fat_execution_statistics().checkpoints>=1);
      CHECK(tree::scan(old,0,10000,v).size()==16*15);
    });
    s->query([&](auto p,uint64_t v){
      CHECK(tree::scan(p,0,10000,v).size()==16*23+1);
      CHECK(tree::find(p,33,v)==99 && tree::find(p,1501,v)==8);
      auto target=tree::interface::concurrent_fat_target(const_cast<tree::nodeptr>(p),1500);
      CHECK((target==untouched)==!full);last=const_cast<tree::nodeptr>(p);
    });
    s->collect_garbage();CHECK(s->gc_statistics().pending_bytes==0);
  }destroy(last);std::printf("native checkpoint full=%u passed\n",full);
}
#endif
void no_gc_snapshot(){
  std::vector<kv> data{{0,1000}};auto root=build(data,1);tree::nodeptr last;
  {
    auto s=std::make_unique<scheduler>(1,1,root,1,false,2);
    bool rejected=false;try{s->query([](auto){return 0;});}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
    s->wait_for_processing(s->update(0,11));
    s->query([&](auto p,uint64_t version){for(auto value:{22,33,44})s->wait_for_processing(s->update(0,value));CHECK(tree::find(p,0,version)==11);});
    s->query([&](auto p,uint64_t v){CHECK(tree::find(p,0,v)==44);last=const_cast<tree::nodeptr>(p);});
  }CHECK(last!=root);destroy(root);destroy(last);std::puts("GC-off snapshot and version callback guard passed");
}
int main(){
  no_gc_snapshot();paused_predecessor();
#ifdef TREE_BETREE
  checkpoint(false);checkpoint(true);
#else
  growth();
#endif
  for(unsigned slots:{2,4,8})capacity(slots);
  for(unsigned slots:{0,2,4,8})for(unsigned pipes:{1,3})for(unsigned workers:{1,2,4})reference(slots,pipes,workers);
}
