#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include "lib/conctrl/concow_cyclic.hpp"
#include "src/adapters/btree.hpp"
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}}while(0)
template<class F> void await(F f) {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  while(!f()){CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
}
struct controlled : btree::interface {
  static inline std::atomic_uint entered{0},released{0},prepared{0};
  static inline std::atomic<nodeptr> successor{nullptr};
  static void before_worker_step(context* ctx) {
    if(ctx->sno<=2 && ctx->op==operand::FAT_APPEND) {
      const unsigned bit=1U<<(ctx->sno-1);
      entered.fetch_or(bit);
      await([&]{return released.load()&bit;});
    }
    if(ctx->op==operand::MATERIALIZE)prepared.fetch_add(1);
  }
  static int pipeline_fat_reserve(context* ctx,uint64_t committed) {
    const int result=btree::interface::pipeline_fat_reserve(ctx,committed);
    if(ctx->sno==4 && result==1)successor.store(ctx->t_cur);
    return result;
  }
  static void reset(){entered=0;released=0;prepared=0;successor=nullptr;}
};
void destroy(btree::nodeptr p) {
  if(p->type!=btree::LEAF)for(unsigned i=0;i<=p->size;++i)destroy(p->chs[i]);
  btree::free_node(p);
}
template<unsigned P> void out_of_order() {
  controlled::reset();kv value{0,1000};auto root=btree::build(1,&value);btree::nodeptr last;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<controlled,P>>(2,root,1,true,2,true,true);
    CHECK(s->update(0,11)==1 && s->update(0,22)==2);
    await([]{return controlled::entered.load()==3;});
    // Even an unrestricted query must not read a pending slot's value.
    CHECK(btree::find(root,0,UINT64_MAX)==1000);
    CHECK(btree::scan(root,0,1,UINT64_MAX)==std::vector<kv>({{0,1000}}));
    controlled::released=2;
    await([&]{return s->fat_execution_statistics().worker_appends==1;});
    CHECK(s->progress().committed==0);
    s->query([](auto p,uint64_t v){CHECK(v==0 && btree::find(p,0,v)==1000);});
    controlled::released=3;s->wait_for_processing(2);
    CHECK(btree::find(root,0,1)==11 && btree::find(root,0,2)==22);
    s->query([&](auto p,uint64_t v){CHECK(v==2 && btree::find(p,0,v)==22);last=const_cast<btree::nodeptr>(p);});
  }
  destroy(last);
  std::printf("worker append P=%u: same leaf overlap and reverse publication passed\n",P);
}
template<unsigned P> void materialization_overlap(bool split) {
  controlled::reset();std::vector<kv> data;
  for(unsigned i=0;i<(split?15U:1U);++i)data.emplace_back(2*i,1000+2*i);
  auto root=btree::build(data.size(),data.data());btree::nodeptr last;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<controlled,P>>(4,root,2,true,2,true,true);
    s->query(0,[&](auto old,uint64_t v){
      CHECK(s->update(1,11)==1 && s->update(3,33)==2);
      await([]{return controlled::entered.load()==3;});
      CHECK(s->update(5,55)==3); // prepare replacement while both values are pending
      await([]{return controlled::prepared.load()==1;});
      CHECK(s->update(1,111)==4); // reserve on unfinished replacement base
      await([]{return controlled::successor.load()!=nullptr;});
      auto next=controlled::successor.load();CHECK(next!=root && !btree::is_cow_ready(next));
      await([&]{return s->fat_execution_statistics().worker_appends==1;});
      CHECK(s->update(7,77)==5 && s->update(9,99)==6); // next generation materialization
      await([]{return controlled::prepared.load()==2;});
      CHECK(s->progress().committed==0);
      CHECK(btree::scan(old,0,100,v)==data);
      CHECK(btree::leaf_delta(root)->count==2); // closed source receives no later reservations
      controlled::released=3;s->wait_for_processing(6);
      CHECK(btree::scan(old,0,100,v)==data);
      s->query(1,[&](auto p,uint64_t version){
        CHECK(version==6);
        for(auto [k,value]:data)CHECK(btree::find(p,k,version)==value);
        for(auto [k,value]:std::vector<kv>{{1,111},{3,33},{5,55},{7,77},{9,99}})
          CHECK(btree::find(p,k,version)==value);
        CHECK(btree::scan(p,0,100,version).size()==data.size()+5);
        last=const_cast<btree::nodeptr>(p);
      });
      CHECK(s->gc_statistics().pending_nodes>0);
    });
    s->collect_garbage();CHECK(s->gc_statistics().pending_nodes==0);
    auto stats=s->fat_execution_statistics();CHECK(stats.worker_appends==4 && stats.worker_materializations==2);
  }
  destroy(last);
  std::printf("worker materialization P=%u split=%u: pending source, successor append, chained merge and GC passed\n",P,split);
}
template<unsigned P> void snapshot_without_gc() {
  kv value{0,1000};auto old=btree::build(1,&value);btree::nodeptr last;
  {
    auto s=std::make_unique<conctrl::concow_cyclic<btree::interface,P>>(1,old,1,false,2,true,true);
    s->wait_for_processing(s->update(0,11));
    s->query([&](auto p,uint64_t version){
      CHECK(version==1);
      for(auto value:{22,33,44})s->wait_for_processing(s->update(0,value));
      CHECK(btree::find(p,0,version)==11);
    });
    s->query([&](auto p,uint64_t v){CHECK(v==4 && btree::find(p,0,v)==44);last=const_cast<btree::nodeptr>(p);});
  }
  CHECK(last!=old);destroy(old);destroy(last);
  std::printf("worker single-thread P=%u: snapshot without GC passed\n",P);
}
int main(){snapshot_without_gc<1>();snapshot_without_gc<3>();out_of_order<1>();out_of_order<3>();materialization_overlap<1>(false);materialization_overlap<3>(false);materialization_overlap<1>(true);materialization_overlap<3>(true);}
