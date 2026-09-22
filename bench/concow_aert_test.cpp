#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <thread>
#include "lib/conctrl/concow_fat.hpp"
#include "radix_walk.hpp"
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
using scheduler=conctrl::concow_fat<aert::interface>;
void destroy(aert::nodeptr p){radix_bench::walk(p,[](auto p){aert::free_node(p);});}
template<class F> void await(F f){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);while(!f()){CHECK(std::chrono::steady_clock::now()<end);std::this_thread::yield();}}
struct operation{uint64_t ticket,key,value;};
struct snapshot{uint64_t version;std::vector<kv> values;};
void reference(unsigned slots,unsigned workers,bool batch) {
  std::vector<kv> data;for(uint64_t i=0;i<256;++i)data.emplace_back(i<<56,i+1000);
  auto root=aert::build(data.size(),data.data());aert::nodeptr final_root;
  {
    scheduler s(root,{.workers=workers,.readers=2,.slots=uint8_t(slots),.gc=true,
      .queue_capacity=31,.batch_size=13,.parallel_min_groups=2,.parallel_materialization=batch});
    std::vector<operation> logs[4];std::vector<snapshot> snaps;std::atomic_bool done=false;
    s.query(0,[&](auto old,uint64_t v){
      std::thread reader([&]{while(!done){s.query(1,[&](auto p,uint64_t version){
        if(snaps.size()<64)snaps.push_back({version,aert::scan(p,0,10000,version)});
      });std::this_thread::yield();}});
      std::vector<std::thread> clients;
      for(unsigned c=0;c<4;++c)clients.emplace_back([&,c]{std::mt19937_64 rng(182+c);
        for(unsigned i=0;i<500;++i){uint64_t key=i%3==0?0:i%3==1?(rng()%256)<<56:rng();
          uint64_t value=10000+c*500+i;logs[c].push_back({s.update(key,value),key,value});}
      });
      for(auto& t:clients)t.join();s.wait_for_processing(2000);done=true;reader.join();
      CHECK(aert::scan(old,0,10000,v)==data);
    });
    s.query([&](auto p,uint64_t v){CHECK(v==2000);snaps.push_back({v,aert::scan(p,0,10000,v)});final_root=const_cast<aert::nodeptr>(p);});
    std::vector<operation> all;for(auto& log:logs)all.insert(all.end(),log.begin(),log.end());
    std::sort(all.begin(),all.end(),[](auto a,auto b){return a.ticket<b.ticket;});
    std::map<uint64_t,uint64_t> expected(data.begin(),data.end());size_t pos=0;
    for(auto& snap:snaps){while(pos<snap.version){CHECK(all[pos].ticket==pos+1);expected[all[pos].key]=all[pos].value;++pos;}
      CHECK(snap.values.size()==expected.size());size_t i=0;for(auto [k,v]:expected)CHECK(snap.values[i++]==kv(k,v));}
    s.collect_garbage();auto gc=s.gc_statistics();CHECK(!gc.pending_bytes && gc.retired_bytes==gc.reclaimed_bytes);
    auto stats=s.execution_statistics();CHECK(stats.appended_updates+stats.cow_updates==2000);
  }
  destroy(final_root);std::printf("AERT slots=%u workers=%u batch=%d passed\n",slots,workers,batch);std::fflush(stdout);
}
void growth_and_empty() {
  aert::nodeptr root;
  {scheduler s(nullptr,{.workers=2,.slots=2,.parallel_materialization=true});
    for(uint64_t i=0;i<256;++i){s.wait_for_processing(s.update(i<<56,i+1));
      s.query([&](auto p,uint64_t v){CHECK(v==i+1);CHECK(aert::scan(p,0,300,v).size()==i+1);
        if(i==4)CHECK(p->type==aert::NODE16);if(i==16)CHECK(p->type==aert::NODEH4);if(i==80)CHECK(p->type==aert::NODEH16);});}
    s.query([&](auto p,uint64_t){root=const_cast<aert::nodeptr>(p);});s.collect_garbage();CHECK(!s.gc_statistics().pending_bytes);
  }destroy(root);
}
void bulk_growth() {
  std::vector<kv> data;for(uint64_t i=0;i<4;++i)data.emplace_back(i<<56,1000+i);
  auto old=aert::build(data.size(),data.data());
  std::vector<aert::fat_replacement> replacements;
  for(uint64_t i=0;i<256;++i)replacements.push_back(aert::prepare_fat_leaf(aert::fat_leaf_for(old,i<<56),1,i<<56,i+1));
  std::vector<aert::fat_replacement*> changes;for(auto& r:replacements)changes.push_back(&r);
  aert::fat_merge merged;merged.build(old,changes,1);
  CHECK(merged.root->type==aert::NODEH16);CHECK(merged.retired_count==5);
  CHECK(std::set<aert::nodeptr>(merged.retired.begin(),merged.retired.end()).size()==5);
  auto result=aert::scan(merged.root,0,300,1);CHECK(result.size()==256);
  for(uint64_t i=0;i<256;++i)CHECK(result[i]==kv(i<<56,i+1));
  CHECK(aert::scan(old,0,300,0)==data);auto root=merged.root;merged.release(changes);
  merged.drain_retired([](auto p){aert::free_node(p);});destroy(root);
}
void prefix_with_future_append() {
  auto old=aert::new_leaf(0,0,42,1000);
  CHECK(((aert::leaf*)old)->fat.append(2,1,1234));
  auto replacement=aert::prepare_fat_leaf(nullptr,2,UINT64_MAX,5678);
  std::vector<aert::fat_replacement*> changes{&replacement};aert::fat_merge merged;merged.build(old,changes,2);
  CHECK(aert::find(merged.root,42,2)==1234);CHECK(aert::find(merged.root,UINT64_MAX,2)==5678);
  CHECK(aert::find(old,42,0)==1000);CHECK(merged.retired_count==1);
  auto root=merged.root;merged.release(changes);merged.drain_retired([](auto p){aert::free_node(p);});destroy(root);
}
struct gated:aert::interface {
  static inline std::atomic_bool first=false,start=false,release=false;
  static inline std::atomic_uint active=0;
  static inline bool fail=false;
  static bool concurrent_fat_append(nodeptr p,uint8_t slots,uint64_t v,uint64_t key,uint64_t value){
    if(v==1){first=true;await([]{return start.load();});}return aert::interface::concurrent_fat_append(p,slots,v,key,value);
  }
  static aert::fat_replacement concurrent_fat_prepare(nodeptr p,uint64_t v,uint64_t key,uint64_t value){
    auto result=aert::interface::concurrent_fat_prepare(p,v,key,value);++active;await([]{return release.load();});
    if(fail && key==2)throw std::runtime_error("injected prepare failure");return result;
  }
};
void overlap(bool fail){
  gated::first=false;gated::start=false;gated::release=false;gated::active=0;gated::fail=fail;
  auto old=aert::new_leaf(0,0,0,1000);aert::nodeptr root;
  {conctrl::concow_fat<gated> s(old,{.workers=2,.readers=2,.slots=2,.parallel_min_groups=2,.parallel_materialization=true});
    s.query(0,[&](auto p,uint64_t version){s.update(0,1);await([]{return gated::first.load();});
      s.update(1,11);s.update(2,22);gated::start=true;await([]{return gated::active.load()==2;});
      s.query(1,[&](auto q,uint64_t v){CHECK(v==1);CHECK(!aert::find(q,1,v));});gated::release=true;
      bool threw=false;try{s.wait_for_processing(3);}catch(const std::runtime_error&){threw=true;}CHECK(threw==fail);
      CHECK(aert::find(p,0,version)==1000);CHECK(!aert::find(p,1,version));});
    s.query([&](auto p,uint64_t v){
      if(fail){CHECK(v==1 && !aert::find(p,1,v) && !aert::find(p,2,v));}
      else CHECK(v==3 && aert::find(p,1,v)==11 && aert::find(p,2,v)==22);
      root=const_cast<aert::nodeptr>(p);});
    CHECK(s.execution_statistics().parallel_materializations==(fail?0:2));s.collect_garbage();CHECK(!s.gc_statistics().pending_bytes);
  }destroy(root);
}
void tagged_paths() {
  std::vector<kv> data;for(uint64_t i=0;i<16;++i)data.emplace_back(i<<60,100+i);
  data.emplace_back(uint64_t(1)<<56,200);std::sort(data.begin(),data.end());
  auto old=aert::build(data.size(),data.data());aert::nodeptr root;
  CHECK(old->type==aert::NODEH16);
  CHECK((uintptr_t(aert::findch(old,1))&0x10)!=0);
  std::map<uint64_t,uint64_t> expected(data.begin(),data.end());
  {scheduler s(old,{.workers=4,.readers=2,.slots=2,.batch_size=64,.parallel_min_groups=2,.parallel_materialization=true});
    s.query(0,[&](auto p,uint64_t v){
      std::vector<kv> updates{{uint64_t(1)<<60,999},{uint64_t(0x11)<<56,300},{(uint64_t(0x10)<<56)+1,301},
        {uint64_t(0x12)<<56,302},{uint64_t(0x13)<<56,303},{uint64_t(0x14)<<56,304}};
      uint64_t ticket=0;for(auto [k,value]:updates){expected[k]=value;ticket=s.update(k,value);}s.wait_for_processing(ticket);
      CHECK(aert::scan(p,0,1000,v)==data);
    });
    s.query([&](auto p,uint64_t v){
      CHECK(aert::scan(p,0,1000,v)==std::vector<kv>(expected.begin(),expected.end()));
      for(auto [k,value]:expected){CHECK(aert::find(p,k,v)==value);
        for(auto probe:{k,k? k-1:k,k==UINT64_MAX?k:k+1}){
          std::vector<kv> want;for(auto it=expected.lower_bound(probe);it!=expected.end() && want.size()<5;++it)want.emplace_back(*it);
          CHECK(aert::scan(p,probe,5,v)==want);
        }
      }
      root=const_cast<aert::nodeptr>(p);
    });s.collect_garbage();CHECK(!s.gc_statistics().pending_bytes);
  }destroy(root);
}
int main(){growth_and_empty();bulk_growth();prefix_with_future_append();overlap(false);overlap(true);tagged_paths();
  for(unsigned slots:{0,2,4,8})for(unsigned workers:{1,2,4})for(bool batch:{false,true})reference(slots,workers,batch);
  std::puts("AERT ordered-wave tests passed");
}
