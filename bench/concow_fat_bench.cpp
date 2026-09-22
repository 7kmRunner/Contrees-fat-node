#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "lib/conctrl/concow_fat.hpp"
#ifdef CONCOW_NATIVE
#include "lib/conctrl/concow_cyclic.hpp"
#endif
#include "src/adapters/btree.hpp"
#include "utils/memory_stats.hpp"

#ifndef CONCOW_FAT_PROFILE
#define CONCOW_FAT_PROFILE 0
#endif
#ifdef CONCOW_NATIVE
#ifndef CONCOW_NATIVE_PIPES
#define CONCOW_NATIVE_PIPES 3
#endif
using controller=conctrl::concow_cyclic<btree::interface,CONCOW_NATIVE_PIPES>;
#else
using controller=conctrl::concow_fat<btree::interface, bool(CONCOW_FAT_PROFILE)>;
#endif
struct request { uint64_t key,value,ticket=0; };
struct live_stats { uint64_t nodes=0,bytes=0,sidecar_bytes=0; };
void inspect(btree::cnodeptr p,live_stats& stats) {
  ++stats.nodes;stats.bytes+=btree::node_size(p);
  if(p->type==btree::LEAF)stats.sidecar_bytes+=btree::node_size(p)-sizeof(btree::node);
  else for(unsigned i=0;i<=p->size;++i)inspect(p->chs[i],stats);
}
void destroy(btree::nodeptr p) {
  if(p->type!=btree::LEAF)for(unsigned i=0;i<=p->size;++i)destroy(p->chs[i]);
  btree::free_node(p);
}
uint64_t number(const char* s) {
  uint64_t n=0;const char* end=s+std::char_traits<char>::length(s);
  auto [p,e]=std::from_chars(s,end,n);
  if(e!=std::errc{} || p!=end)throw std::invalid_argument("invalid integer");return n;
}
int main(int argc,char** argv) {
  try {
    uint64_t records=10000,updates=30000,clients=4,workers=2,slots=2,parallel_min_groups=16;
    std::string pattern="uniform";bool pinned=false,materialize=false,wait_each=false,local_probe=true,worker_fat=false;
    for(int i=1;i<argc;++i) {
      const std::string arg=argv[i];
      if(arg=="--fat-workers") { worker_fat=true;continue; }
      if(arg=="--global-probe") { local_probe=false;continue; }
      if(arg=="--wait-each") { wait_each=true;continue; }
      if(arg=="--parallel-materialization") { materialize=true;continue; }
      if(arg=="--pinned") { pinned=true;continue; }
      if(++i==argc)throw std::invalid_argument("missing argument value");
      if(arg=="--records")records=number(argv[i]);
      else if(arg=="--updates")updates=number(argv[i]);
      else if(arg=="--clients")clients=number(argv[i]);
      else if(arg=="--workers")workers=number(argv[i]);
      else if(arg=="--slots")slots=number(argv[i]);
      else if(arg=="--parallel-min-groups")parallel_min_groups=number(argv[i]);
      else if(arg=="--pattern")pattern=argv[i];
      else throw std::invalid_argument("unknown option");
    }
    if(!records || !updates || !clients || !workers || workers>64 || clients>64 ||
       updates>UINT32_MAX || (slots!=0 && slots!=2 && slots!=4 && slots!=8) ||
       (pattern!="uniform" && pattern!="hot" && pattern!="zipfian"))throw std::invalid_argument("invalid benchmark options");
#ifndef CONCOW_NATIVE
    if(worker_fat)throw std::invalid_argument("--fat-workers requires native cyclic ConCow");
#endif
    if(worker_fat && (!local_probe || !slots))throw std::invalid_argument("--fat-workers requires local probe and nonzero slots");
    std::vector<kv> data(records);
    for(uint64_t i=0;i<records;++i)data[i]={i,0};
    std::vector<request> requests(updates);
    std::mt19937_64 random(182);
    std::vector<double> cdf;
    std::vector<uint64_t> zipf_keys;
    if(pattern=="zipfian") {
      double sum=0;
      for(uint64_t i=0;i<records;++i){cdf.push_back(sum+=std::pow(double(i+1),-.99));zipf_keys.push_back(i);}
      std::shuffle(zipf_keys.begin(),zipf_keys.end(),random);
    }
    for(uint64_t i=0;i<updates;++i) {
      uint64_t key;
      if(pattern=="zipfian") {
        const double u=(random()>>11)*0x1.0p-53*cdf.back();
        const size_t rank=std::min<size_t>(records-1,std::upper_bound(cdf.begin(),cdf.end(),u)-cdf.begin());
        key=zipf_keys[rank];
      } else if(pattern=="hot" && random()%10<9)key=random()%std::min<uint64_t>(100,records);
      else key=random()%records;
      requests[i]={key,i+1,0};
    }
    // Fixed-size reference model is present in every sample, irrespective of
    // slot count. Last-writer order comes from returned tickets, not client IDs.
    std::vector<std::pair<uint64_t,uint64_t>> expected(records,{0,0});
    auto initial=btree::build(data.size(),data.data());btree::nodeptr final_root;
    {
#ifdef CONCOW_NATIVE
      if(materialize)throw std::invalid_argument("native pipeline has no batch merge mode");
      auto native_owner=std::make_unique<controller>(unsigned(workers),initial,1,true,uint8_t(slots),local_probe,worker_fat);
      auto& s=*native_owner;
#else
      controller s(initial,{.workers=unsigned(workers),.readers=1,.slots=uint8_t(slots),.gc=true,.parallel_min_groups=size_t(parallel_min_groups),.parallel_materialization=materialize});
#endif
      auto execute=[&] {
        std::atomic_uint64_t cursor{0};
        std::vector<std::thread> writers;
        for(uint64_t c=0;c<clients;++c)writers.emplace_back([&] {
          for(;;) {
            const auto first=cursor.fetch_add(64);
            if(first>=updates)break;
            for(auto i=first;i<std::min<uint64_t>(first+64,updates);++i) {
              requests[i].ticket=s.update(requests[i].key,requests[i].value);
              if(wait_each)s.wait_for_processing(requests[i].ticket);
            }
          }
        });
        for(auto& writer:writers)writer.join();
        s.wait_for_processing(updates);
      };
      const auto begin=std::chrono::steady_clock::now();
      if(pinned)s.query([&](auto p,uint64_t v) {
        execute();
        if(v!=0 || btree::find(p,0,v)!=0)throw std::runtime_error("pinned snapshot changed");
      });
      else execute();
      const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
      s.collect_garbage();
      for(const auto& op:requests)if(expected[op.key].first<op.ticket)expected[op.key]={op.ticket,op.value};
      live_stats live;
      s.query([&](auto p,uint64_t v) {
        if(v!=updates)throw std::runtime_error("incomplete committed prefix");
        for(uint64_t k=0;k<records;++k)if(btree::find(p,k,v)!=expected[k].second)throw std::runtime_error("reference model mismatch");
        inspect(p,live);final_root=const_cast<btree::nodeptr>(p);
      });
      const auto gc=s.gc_statistics();
#ifdef CONCOW_NATIVE
      const auto native=s.fat_execution_statistics();
      const conctrl::concow_fat<btree::interface>::statistics stats{
        native.appended,slots?native.structural:updates,0,0,0,0,0,0};
      const conctrl::concow_fat<btree::interface>::phase_statistics phases{};
#else
      const auto stats=s.execution_statistics();const auto phases=s.phase_timings();
#endif
      if(gc.pending_bytes || gc.pending_nodes || gc.retired_bytes!=gc.reclaimed_bytes)throw std::runtime_error("GC failed to drain");
      const auto mem=contrees::memory_stats::sample_now();
      std::cout<<"{\"records\":"<<records<<",\"updates\":"<<updates<<",\"clients\":"<<clients
        <<",\"wait_each\":"<<wait_each
#ifdef CONCOW_NATIVE
        <<",\"native_pipeline\":true,\"local_probe\":"<<local_probe<<",\"blocked_probes\":"<<native.blocked_probes
        <<",\"worker_fat\":"<<worker_fat<<",\"worker_appends\":"<<native.worker_appends
        <<",\"worker_materializations\":"<<native.worker_materializations
        <<",\"pipeline_threads\":"<<(CONCOW_NATIVE_PIPES+1)
#else
        <<",\"native_pipeline\":false"
#endif
        <<",\"workers\":"<<workers<<",\"slots\":"<<slots<<",\"pattern\":\""<<pattern<<"\",\"pinned\":"<<pinned
        <<",\"elapsed_seconds\":"<<elapsed<<",\"peak_rss_bytes\":"<<mem.peak_rss_bytes
        <<",\"live_requested_bytes\":"<<live.bytes<<",\"sidecar_bytes\":"<<live.sidecar_bytes
        <<",\"retired_bytes\":"<<gc.retired_bytes<<",\"reclaimed_bytes\":"<<gc.reclaimed_bytes
        <<",\"retired_nodes\":"<<gc.retired_nodes
        <<",\"pending_bytes\":"<<gc.pending_bytes<<",\"appended_updates\":"<<stats.appended_updates
        <<",\"cow_updates\":"<<stats.cow_updates<<",\"append_waves\":"<<stats.append_waves
        <<",\"max_wave_groups\":"<<stats.max_wave_groups
        <<",\"parallel_min_groups\":"<<parallel_min_groups<<",\"parallel_waves\":"<<stats.parallel_waves
        <<",\"parallel_materialization\":"<<materialize
        <<",\"materialization_waves\":"<<stats.materialization_waves
        <<",\"parallel_materializations\":"<<stats.parallel_materializations
        <<",\"parallel_updates\":"<<stats.parallel_updates
        <<",\"profile\":"<<CONCOW_FAT_PROFILE
        <<",\"queue_ns\":"<<phases.queue_ns<<",\"plan_ns\":"<<phases.plan_ns
        <<",\"inline_leaf_ns\":"<<phases.inline_leaf_ns
        <<",\"parallel_wave_ns\":"<<phases.parallel_wave_ns
        <<",\"merge_retire_ns\":"<<phases.merge_retire_ns
        <<",\"publish_ns\":"<<phases.publish_ns<<",\"gc_ns\":"<<phases.gc_ns
        <<",\"serial_cow_ns\":"<<phases.serial_cow_ns
        <<",\"worker_loop_ns\":"<<phases.worker_loop_ns
        <<",\"worker_wakeups\":"<<phases.worker_wakeups
        <<",\"reference_ok\":true}\n";
    }
    destroy(final_root);return 0;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
