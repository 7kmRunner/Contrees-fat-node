#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "lib/conctrl/concow_fat.hpp"
#include "src/adapters/betree.hpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
using scheduler = conctrl::concow_fat<betree::interface>;

betree::nodeptr build(unsigned n) {
  std::vector<kv> data;
  for (unsigned i=0; i<n; ++i) data.emplace_back(i, i+10000);
  return betree::build(data.size(), 2, data.data());
}
void free_tree(betree::nodeptr root) {
  if (root->type != betree::LEAF) for(unsigned i=0; i<=root->size; ++i) free_tree(root->chs[i]);
  betree::free_node(root);
}
template<class Scheduler> auto root_of(Scheduler& s) {
  return s.query([](auto root,uint64_t) { return const_cast<betree::nodeptr>(root); });
}
template<class F> void await(F&& predicate) {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(!predicate()) { CHECK(std::chrono::steady_clock::now()<deadline); std::this_thread::yield(); }
}

void capacity_test(unsigned slots) {
  auto initial=build(1);
  betree::nodeptr final_root;
  {
    scheduler s(initial,{.workers=4,.readers=1,.slots=uint8_t(slots),.gc=true,.queue_capacity=8,.batch_size=8});
    s.query([&](auto pinned,uint64_t version) {
      CHECK(version==0);
      for(unsigned i=1;i<=slots;++i) {
        s.wait_for_processing(s.update(0,i));
        // Nested query with the SAME reader id would unpin the outer guard;
        // inspect the already pinned root instead, using known versions.
        CHECK(betree::find(pinned,0,i)==i);
        CHECK(betree::find(pinned,0,0)==10000);
        CHECK(betree::node_size(pinned)==256+betree::delta_block_size(slots));
      }
      s.wait_for_processing(s.update(0,slots+1));
      CHECK(betree::find(pinned,0,0)==10000);
      CHECK(betree::find(pinned,0,slots)==slots);
      CHECK(s.gc_statistics().pending_nodes>0);
    });
    s.collect_garbage();
    CHECK(s.gc_statistics().pending_nodes==0);
    final_root=root_of(s);CHECK(final_root!=initial);
    s.query([&](auto p,uint64_t v) { CHECK(v==slots+1); CHECK(betree::find(p,0,v)==slots+1); });
    auto stats=s.execution_statistics();
    CHECK(stats.appended_updates==slots && stats.cow_updates==1);
  }
  free_tree(final_root);
}

struct logged_update { uint64_t ticket,key,value; };
struct observed { uint64_t version; std::vector<kv> values; };

void reference_test(unsigned slots,unsigned workers,bool materialize=false) {
  constexpr unsigned INITIAL=128, CLIENTS=4, PER_CLIENT=500;
  scheduler::options opts{.workers=workers,.readers=2,.slots=uint8_t(slots),.gc=true,.queue_capacity=31,.batch_size=13,.parallel_min_groups=2,.parallel_materialization=materialize};
  auto initial=build(INITIAL);
  betree::nodeptr final_root;
  {
    scheduler s(initial,opts);
    std::vector<logged_update> histories[CLIENTS];
    std::vector<observed> snapshots;
    std::atomic_bool done=false;
    s.query(0,[&](auto pinned,uint64_t version) {
      CHECK(version==0);
      const auto old=betree::scan(pinned,0,10000,version);
      std::thread reader([&] {
        while(!done.load(std::memory_order_acquire)) {
          s.query(1,[&](auto p,uint64_t v) {
            if(snapshots.size()<256) snapshots.push_back({v,betree::scan(p,0,10000,v)});
            auto value=betree::find(p,0,v);CHECK(value.has_value());
          });
          std::this_thread::yield();
        }
      });
      std::vector<std::thread> writers;
      for(unsigned client=0;client<CLIENTS;++client) writers.emplace_back([&,client] {
        std::mt19937_64 random(992+client);
        for(unsigned i=0;i<PER_CLIENT;++i) {
          uint64_t key=i%3==0 ? 0 : (i%3==1 ? random()%INITIAL : INITIAL+client*PER_CLIENT+i);
          const uint64_t value=100000+client*PER_CLIENT+i;
          const uint64_t ticket=s.update(key,value);
          histories[client].push_back({ticket,key,value});
        }
      });
      for(auto& writer:writers)writer.join();
      s.wait_for_processing(CLIENTS*PER_CLIENT);
      done.store(true,std::memory_order_release);reader.join();
      CHECK(betree::scan(pinned,0,10000,version)==old);
      CHECK(s.gc_statistics().pending_nodes>0);
    });
    std::vector<logged_update> updates;
    for(auto& history:histories)updates.insert(updates.end(),history.begin(),history.end());
    std::sort(updates.begin(),updates.end(),[](auto a,auto b){return a.ticket<b.ticket;});
    for(size_t i=0;i<updates.size();++i)CHECK(updates[i].ticket==i+1);
    s.query([&](auto p,uint64_t v){snapshots.push_back({v,betree::scan(p,0,10000,v)});});
    std::map<uint64_t,uint64_t> model;
    for(unsigned i=0;i<INITIAL;++i)model[i]=i+10000;
    size_t applied=0;
    for(const auto& snap:snapshots) {
      while(applied<snap.version){const auto& op=updates.at(applied++);model[op.key]=op.value;}
      CHECK(snap.values.size()==model.size());
      CHECK(std::equal(snap.values.begin(),snap.values.end(),model.begin(),[](auto a,auto b){return a.first==b.first && a.second==b.second;}));
    }
    s.collect_garbage();auto gc=s.gc_statistics();
    CHECK(gc.pending_nodes==0 && gc.pending_bytes==0 && gc.retired_bytes==gc.reclaimed_bytes);
    auto stats=s.execution_statistics();CHECK(stats.appended_updates+stats.cow_updates==updates.size());
    final_root=root_of(s);
  }
  free_tree(final_root);
  std::printf("reference slots=%u workers=%u passed\n",slots,workers);
}

// Gates expose actual overlap without relying on throughput as proof. Only this
// test adapter pauses workers; no timing hooks are in the production controller.
struct gated_interface : betree::interface {
  static inline std::atomic_bool first_entered{false}, release_first{false}, parallel_seen{false};
  static inline std::atomic_uint active{0};
  static inline std::atomic_bool pause_after_publish{false}, payload_published{false}, release_publish{false};
  static bool concurrent_fat_append(nodeptr p,uint8_t slots,uint64_t v,uint64_t key,uint64_t value) {
    if(v==1) {
      first_entered=true;
      await([]{return release_first.load();});
    } else {
      if(active.fetch_add(1)+1>=2)parallel_seen=true;
      await([]{return parallel_seen.load();});
    }
    const bool result=betree::interface::concurrent_fat_append(p,slots,v,key,value);
    if(v!=1)active.fetch_sub(1);
    if(v==1 && pause_after_publish) {
      payload_published=true;
      await([]{return release_publish.load();});
    }
    return result;
  }
};

void overlap_and_visibility_test(bool gc_enabled) {
  using parallel_scheduler=conctrl::concow_fat<gated_interface>;
  gated_interface::first_entered=false;gated_interface::release_first=false;gated_interface::parallel_seen=false;
  gated_interface::active=0;gated_interface::payload_published=false;gated_interface::release_publish=false;
  gated_interface::pause_after_publish=true;
  auto initial=build(256);
  {
    // Sixteen independent groups exercise the default parallel threshold.
    parallel_scheduler s(initial,{.workers=2,.readers=1,.slots=2,.gc=gc_enabled,.queue_capacity=32,.batch_size=32});
    CHECK(s.update(0,1)==1);
    await([]{return gated_interface::first_entered.load();});
    for(unsigned i=1;i<=16;++i)CHECK(s.update(i*8,i+1)==i+1);
    gated_interface::release_first=true;
    await([]{return gated_interface::payload_published.load();});
    s.query([&](auto p,uint64_t v) {
      CHECK(p==initial && v==0);
      CHECK(betree::find(p,0,v)==10000);
      CHECK(betree::find(p,0,1)==1);
    });
    gated_interface::release_publish=true;
    s.wait_for_processing(17);
    CHECK(gated_interface::parallel_seen);
    CHECK(root_of(s)==initial);
    auto stats=s.execution_statistics();
    CHECK(stats.max_wave_groups==16 && stats.appended_updates==17 && stats.cow_updates==0);
    CHECK(stats.parallel_waves==1 && stats.parallel_updates==16);
    s.query([&](auto p,uint64_t v){CHECK(v==17);CHECK(betree::find(p,128,v)==17);});
  }
  free_tree(initial);
}

struct barrier_interface : betree::interface {
  static inline std::atomic_bool last_slot_written{false}, release_last_slot{false}, cow_started{false};
  static bool concurrent_fat_append(nodeptr p,uint8_t slots,uint64_t v,uint64_t key,uint64_t value) {
    bool ok=betree::interface::concurrent_fat_append(p,slots,v,key,value);
    if(v==2) {
      last_slot_written=true;
      await([]{return release_last_slot.load();});
    }
    return ok;
  }
  static operand concurrent_fat_cow_begin(context* ctx) {
    CHECK(release_last_slot.load());
    cow_started=true;
    return betree::interface::concurrent_fat_cow_begin(ctx);
  }
};
void append_materialization_barrier_test() {
  auto initial=build(64);betree::nodeptr final_root;
  {
    conctrl::concow_fat<barrier_interface> s(initial,{.workers=4,.readers=1,.slots=2,.gc=true,.queue_capacity=8,.batch_size=8});
    s.wait_for_processing(s.update(0,1));
    s.query([&](auto pinned,uint64_t version) {
      CHECK(version==1);
      CHECK(s.update(0,2)==2);
      await([]{return barrier_interface::last_slot_written.load();});
      CHECK(s.update(0,3)==3);CHECK(s.update(32,4)==4);
      CHECK(!barrier_interface::cow_started.load());
      CHECK(betree::find(pinned,0,version)==1);
      barrier_interface::release_last_slot=true;
      s.wait_for_processing(4);
      CHECK(barrier_interface::cow_started.load());
      CHECK(betree::find(pinned,0,version)==1);
      CHECK(betree::find(pinned,0,2)==2);
      CHECK(s.gc_statistics().pending_nodes>0);
    });
    s.collect_garbage();CHECK(s.gc_statistics().pending_nodes==0);
    s.query([&](auto p,uint64_t v){CHECK(v==4);CHECK(betree::find(p,0,v)==3);CHECK(betree::find(p,32,v)==4);});
    final_root=root_of(s);
  }
  free_tree(final_root);
}

struct failing_interface : betree::interface {
  static inline std::atomic_bool entered{false}, release{false};
  static bool concurrent_fat_append(nodeptr p,uint8_t slots,uint64_t v,uint64_t key,uint64_t value) {
    if(v==1) { entered=true; await([]{return release.load();}); }
    if(v==3) throw std::runtime_error("injected worker failure");
    return betree::interface::concurrent_fat_append(p,slots,v,key,value);
  }
};
void worker_failure_test() {
  auto root=build(64);
  {
    conctrl::concow_fat<failing_interface> s(root,{.parallel_min_groups=2});
    CHECK(s.update(0,1)==1);
    await([]{return failing_interface::entered.load();});
    CHECK(s.update(16,22)==2);CHECK(s.update(32,33)==3);
    failing_interface::release=true;
    bool threw=false;try{s.wait_for_processing(3);}catch(const std::runtime_error&){threw=true;}
    CHECK(threw);threw=false;
    try{s.update(0,2);}catch(const std::runtime_error&){threw=true;}CHECK(threw);
    s.query([&](auto p,uint64_t v){
      CHECK(v==1 && betree::find(p,0,v)==1);
      CHECK(betree::find(p,16,v)==10016); // successful partial wave is invisible
      CHECK(betree::find(p,16,2)==22);
    });
  }
  free_tree(root);
}
void drain_and_validation_test() {
  auto root=build(64);
  {
    scheduler s(root,{.workers=4,.readers=1,.slots=8,.gc=true,.queue_capacity=2,.batch_size=2});
    for(unsigned i=0;i<8;++i)s.update(i*8,42);
    // No explicit wait: destructor must finish every accepted append.
  }
  for(unsigned i=0;i<8;++i)CHECK(betree::find(root,i*8,8)==42);
  free_tree(root);
  bool threw=false;
  try{scheduler invalid(nullptr,{.workers=0});}catch(const std::invalid_argument&){threw=true;}CHECK(threw);
}
struct materialization_gate : betree::interface {
  static inline std::atomic_bool entered=false, start=false, release=false;
  static inline std::atomic_uint active=0;
  static inline bool fail=false;
  static bool concurrent_fat_append(nodeptr p,uint8_t slots,uint64_t v,uint64_t key,uint64_t value) {
    if(v==1) {entered=true;await([]{return start.load();});}
    return betree::interface::concurrent_fat_append(p,slots,v,key,value);
  }
  static betree::fat_replacement concurrent_fat_prepare(nodeptr p,uint64_t v,uint64_t key,uint64_t value) {
    auto result=betree::interface::concurrent_fat_prepare(p,v,key,value);
    active.fetch_add(1);
    await([]{return release.load();});
    if(fail && key==16) throw std::runtime_error("injected materialization failure");
    return result;
  }
};
void materialization_overlap_test(bool fail) {
  materialization_gate::entered=false; materialization_gate::start=false;
  materialization_gate::release=false; materialization_gate::active=0; materialization_gate::fail=fail;
  auto initial=build(256); auto final_root=initial;
  {
    conctrl::concow_fat<materialization_gate> s(initial,{.workers=2,.readers=2,.slots=2,.gc=true,
      .queue_capacity=64,.batch_size=64,.parallel_min_groups=2,.parallel_materialization=true});
    s.query(0,[&](auto old,uint64_t old_version) {
      CHECK(s.update(0,1)==1);
      await([]{return materialization_gate::entered.load();});
      for(unsigned i=1;i<=16;++i)for(unsigned n=0;n<3;++n)s.update(i*8,100+n);
      materialization_gate::start=true;
      await([]{return materialization_gate::active.load()>=2;});
      // Two workers hold fully prepared private leaves; neither is reachable.
      s.query(1,[&](auto p,uint64_t v){CHECK(p==initial && v==1);CHECK(betree::find(p,8,v)==10008);});
      CHECK(s.gc_statistics().retired_nodes==0);
      s.collect_garbage();
      materialization_gate::release=true;
      bool threw=false;try{s.wait_for_processing(49);}catch(const std::runtime_error&){threw=true;}
      CHECK(threw==fail);
      CHECK(betree::find(old,8,old_version)==10008);
      s.query(1,[&](auto p,uint64_t v){
        CHECK(v==(fail?1:49));
        for(unsigned i=1;i<=16;++i)CHECK(betree::find(p,i*8,v)==(fail?10000+i*8:102));
        final_root=const_cast<betree::nodeptr>(p);
      });
      if(!fail) {
        const auto stats=s.execution_statistics();
        CHECK(stats.cow_updates==16 && stats.parallel_materializations==16);
        CHECK(stats.materialization_waves==1 && stats.appended_updates==33);
        CHECK(s.gc_statistics().pending_nodes>0);
      }
    });
    s.collect_garbage();CHECK(s.gc_statistics().pending_nodes==0);
  }
  free_tree(final_root);
}

// Force simultaneous splits in all 256 leaves, both internal levels, and root.
void merge_cascade_test() {
  std::vector<betree::fat_replacement> replacements;
  std::vector<betree::fat_replacement*> changes;
  std::map<uint64_t,uint64_t> expected;
  auto old=betree::new_internal(0,15);
  for(unsigned i=0;i<16;++i) {
    auto parent=betree::new_internal(0,15);old->chs[i]=parent;
    if(i)old->keys[i-1]=i*1600;
    for(unsigned j=0;j<16;++j) {
      unsigned base=i*1600+j*100;
      auto leaf=betree::new_leaf(0,15);parent->chs[j]=leaf;
      if(j)parent->keys[j-1]=base;
      for(unsigned k=0;k<15;++k){leaf->keys[k]=base+4*k;leaf->vals[k]=7;expected[base+4*k]=7;}
      for(unsigned k=0;k<8;++k){CHECK(betree::append_leaf_delta(leaf,8,k+1,base+4*k+1,8));expected[base+4*k+1]=8;}
      expected[base+33]=9;
      replacements.push_back(betree::prepare_fat_leaf(leaf,9,base+33,9));
    }
  }
  for(auto& r:replacements)changes.push_back(&r);
  betree::fat_merge merged;merged.build(old,changes,9);
  CHECK(merged.retired_count==273);
  CHECK(merged.root->type==betree::LOWER && merged.root->size==1);
  auto values=betree::scan(merged.root,0,100000,9);
  CHECK(values.size()==expected.size());
  size_t index=0;for(auto [key,value]:expected)CHECK(values[index++]==kv(key,value));
  CHECK(betree::scan(old,0,100000,0).size()==256*15);
  auto current=merged.root;merged.release(changes);
  merged.drain_retired([](auto p){betree::free_node(p);});
  free_tree(current);
}

struct checkpoint_observer : betree::interface {
  static inline unsigned checkpoints=0;
  static inline bool expected_full=false;
  static std::pair<uint64_t,uint64_t> checkpoint_retired_stats(nodeptr p) {
    CHECK(betree::leaf_over_boundary(p,p->verge)==expected_full);
    ++checkpoints;
    return betree::checkpoint_retired_stats(p);
  }
};

void boundary_checkpoint_test(bool full,bool batch) {
  checkpoint_observer::checkpoints=0;checkpoint_observer::expected_full=full;
  auto boundary=betree::new_internal(0,0);boundary->type=betree::BOUNDARY;boundary->verge=full?3:0;
  auto lower=betree::new_internal(0,15);lower->type=betree::LOWER;lower->height=betree::E;
  boundary->chs[0]=lower;
  for(unsigned i=0;i<16;++i) {
    const uint64_t base=i*100;
    if(i)lower->keys[i-1]=base;
    auto leaf=betree::new_leaf(0,15);lower->chs[i]=leaf;
    for(unsigned k=0;k<15;++k){leaf->keys[k]=base+4*k;leaf->vals[k]=7;}
    for(unsigned k=0;k<8;++k)CHECK(betree::append_leaf_delta(leaf,8,1,base+4*k+1,8));
  }
  betree::nodeptr final_root;
  const auto untouched=lower->chs[15];
  {
    conctrl::concow_fat<checkpoint_observer> s(boundary,{.workers=4,.readers=1,.slots=8,.gc=true,
      .queue_capacity=32,.batch_size=32,.parallel_min_groups=2,.parallel_materialization=batch});
    s.query([&](auto old,uint64_t old_version) {
      s.wait_for_processing(s.update(33,9));
      CHECK(checkpoint_observer::checkpoints==1);
      CHECK(betree::scan(old,0,1000,old_version).size()==16*15);
      CHECK(s.gc_statistics().pending_nodes>0);
    });
    s.query([&](auto p,uint64_t version) {
      CHECK(version==1);
      const auto values=betree::scan(p,0,1000,version);
      CHECK(values.size()==16*23+1);
      CHECK(betree::find(p,33,version)==9);
      auto shared=betree::interface::concurrent_fat_target(const_cast<betree::nodeptr>(p),1500);
      CHECK((shared==untouched)==!full);
      final_root=const_cast<betree::nodeptr>(p);
    });
    s.collect_garbage();auto gc=s.gc_statistics();
    CHECK(!gc.pending_nodes && gc.retired_bytes==gc.reclaimed_bytes);
    s.query([&](auto p,uint64_t v){CHECK(betree::find(p,1501,v)==8);CHECK(betree::find(p,1500,v)==7);});
  }
  free_tree(final_root);
}

int main() {
  materialization_overlap_test(false);materialization_overlap_test(true);merge_cascade_test();
  for(bool full:{false,true})for(bool batch:{false,true})boundary_checkpoint_test(full,batch);
  for(unsigned slots:{2,4,8})capacity_test(slots);
  overlap_and_visibility_test(true);overlap_and_visibility_test(false);
  append_materialization_barrier_test();worker_failure_test();drain_and_validation_test();
  for(unsigned slots:{0,2,4,8})for(unsigned workers:{1,2,4}){reference_test(slots,workers);reference_test(slots,workers,true);}
  std::puts("experimental ConCow fat BeTree tests passed");
}
