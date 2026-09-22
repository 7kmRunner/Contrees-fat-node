// Characterization of an inherited progress limitation, NOT a lock-free pass.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include "lib/conctrl/concow_cyclic.hpp"
#include "src/adapters/btree.hpp"
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
template<class F> void await(F f) {
  const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(!f()){CHECK(std::chrono::steady_clock::now()<limit);std::this_thread::yield();}
}
struct paused_submission : btree::interface {
  static inline std::atomic_bool entered=false,release=false,ring_full=false;
  static inline std::atomic_uint64_t monitor_passes=0;
  static void on_submission_reserved(uint64_t ticket) {
    if(ticket==1){entered=true;await([]{return release.load();});}
    if(ticket==conctrl::BUFFER_SIZE)ring_full=true;
  }
  static void on_monitor_pass(){monitor_passes.fetch_add(1,std::memory_order_relaxed);}
};
void admission_hole(unsigned slots) {
  paused_submission::entered=false;paused_submission::release=false;
  paused_submission::ring_full=false;paused_submission::monitor_passes=0;
  kv data{0,0};auto root=btree::build(1,&data);btree::nodeptr last;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<paused_submission,1>>(2,root,1,true,slots);
    std::thread owner([&]{CHECK(s->update(0,1)==1);});
    await([]{return paused_submission::entered.load();});
    for(uint64_t i=2;i<conctrl::BUFFER_SIZE;++i)CHECK(s->update(0,i)==i);
    std::atomic_bool returned=false;
    std::thread producer([&]{CHECK(s->update(0,conctrl::BUFFER_SIZE)==conctrl::BUFFER_SIZE);returned=true;});
    await([]{return paused_submission::ring_full.load();});
    const auto start=paused_submission::monitor_passes.load();
    await([&]{return paused_submission::monitor_passes.load()>=start+10000;});
    const auto p=s->progress();
    CHECK(p.submitted==conctrl::BUFFER_SIZE && p.initialized==0 && p.committed==0);
    CHECK(!returned.load());
    s->query([](auto p,uint64_t v){CHECK(v==0 && btree::find(p,0,v)==0);});
    paused_submission::release=true;owner.join();producer.join();
    s->wait_for_processing(conctrl::BUFFER_SIZE);
    s->query([&](auto p,uint64_t v){CHECK(v==conctrl::BUFFER_SIZE && btree::find(p,0,v)==v);last=const_cast<btree::nodeptr>(p);});
    s->collect_garbage();CHECK(s->gc_statistics().pending_bytes==0);
  }
  btree::free_node(last);
  std::printf("slots=%u: reproduced admission stall with paused ticket owner; resumed and verified\n",slots);
}
int main(){admission_hole(0);admission_hole(2);}
