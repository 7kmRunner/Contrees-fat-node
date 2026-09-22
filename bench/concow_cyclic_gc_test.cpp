#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include "lib/conctrl/concow_cyclic.hpp"
#include "src/adapters/btree.hpp"
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
template<class F> void await(F f) {
  const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  while(!f()){CHECK(std::chrono::steady_clock::now()<limit);std::this_thread::yield();}
}
struct observed_gc : btree::interface {
  static inline thread_local bool monitor_thread=false;
  static inline std::atomic_bool pause_next=false,entered=false,release=false,wrong_owner=false;
  static inline std::atomic_uint64_t freed=0;
  static void on_monitor_pass(){monitor_thread=true;}
  static void on_gc_requested(uint64_t) {
    if(pause_next.exchange(false)){entered=true;await([]{return release.load();});}
  }
  static void free_node(nodeptr p) {
    if(!monitor_thread)wrong_owner=true;
    ++freed;btree::free_node(p);
  }
};
void destroy(btree::nodeptr p) {
  if(p->type!=btree::LEAF)for(unsigned i=0;i<=p->size;++i)destroy(p->chs[i]);
  btree::free_node(p);
}
template<unsigned P> void concurrent_collection(unsigned slots,bool worker_fat=false) {
  observed_gc::pause_next=false;observed_gc::entered=false;
  observed_gc::release=false;observed_gc::wrong_owner=false;observed_gc::freed=0;
  std::vector<kv> data;for(unsigned k=0;k<4096;++k)data.emplace_back(k,k+1000);
  auto root=btree::build(data.size(),data.data());btree::nodeptr last;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<observed_gc,P>>(2,root,2,true,slots,true,worker_fat);
    s->collect_garbage(); // also acknowledge an idle, version-zero request
    s->query(0,[&](auto old,uint64_t version){
      CHECK(version==0);
      observed_gc::pause_next=true;
      std::atomic_bool paused_returned=false;
      std::thread paused([&]{s->collect_garbage();paused_returned=true;});
      await([]{return observed_gc::entered.load();});
      std::atomic_uint done=0;
      std::vector<std::thread> threads;
      for(unsigned c=0;c<2;++c)threads.emplace_back([&,c]{
        uint64_t ticket=0;
        for(unsigned i=1;i<=2000;++i)ticket=s->update(c,i);
        s->wait_for_processing(ticket);++done;
      });
      for(unsigned c=0;c<3;++c)threads.emplace_back([&]{
        for(unsigned i=0;i<100;++i)s->collect_garbage();
        ++done;
      });
      threads.emplace_back([&]{
        for(unsigned i=0;i<200;++i)s->query(1,[](auto p,uint64_t v){
          CHECK(btree::find(p,0,v).has_value() && btree::find(p,1,v).has_value());
        });++done;
      });
      await([&]{return done.load()==6;});
      for(auto& thread:threads)thread.join();
      CHECK(s->progress().committed==4000 && !paused_returned.load());
      CHECK(observed_gc::freed.load()==0); // pinned v0 prevents reclamation
      CHECK(s->gc_statistics().pending_nodes>0);
      for(unsigned k=0;k<4096;++k)CHECK(btree::find(old,k,version)==k+1000);
      observed_gc::release=true;paused.join();
    });
    // No further update is submitted: an explicit idle pass must see unpin.
    s->collect_garbage();
    auto stats=s->gc_statistics();
    CHECK(stats.pending_nodes==0 && stats.pending_bytes==0);
    CHECK(stats.retired_bytes==stats.reclaimed_bytes);
    CHECK(observed_gc::freed.load()==stats.reclaimed_nodes);
    CHECK(!observed_gc::wrong_owner.load());
    s->query([&](auto p,uint64_t v){
      CHECK(v==4000 && btree::find(p,0,v)==2000 && btree::find(p,1,v)==2000);
      last=const_cast<btree::nodeptr>(p);
    });
  }
  destroy(last);
  std::printf("monitor-owned GC P=%u slots=%u worker_fat=%u: concurrent collectors, paused caller, snapshots, idle drain passed\n",P,slots,worker_fat);
}
int main(){for(unsigned slots:{0,2,4,8}){concurrent_collection<1>(slots);concurrent_collection<3>(slots);concurrent_collection<1>(slots,true);concurrent_collection<3>(slots,true);}}
