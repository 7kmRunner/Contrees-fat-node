// Compile one driver against correctness-patched original or current headers.
// Both modes disable GC: the original repository has no reclamation support.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include <sys/resource.h>
#if defined(TREE_BTREE)
#include "src/adapters/btree.hpp"
namespace tree = btree;
#elif defined(TREE_ART)
#include "src/adapters/art.hpp"
namespace tree = art;
#elif defined(TREE_AERT)
#include "src/adapters/aert.hpp"
namespace tree = aert;
#else
#include "src/adapters/betree.hpp"
namespace tree = betree;
#endif
#ifdef ORIGINAL_TREE
#if defined(TREE_BTREE)
#include "lib/conctrl/concow_cyclic.hpp"
using controller = conctrl::concow_cyclic<tree::interface, 3>;
#else
#include "lib/conctrl/concow.hpp"
using controller = conctrl::concow<tree::interface>;
#endif
#else
#include "lib/conctrl/concow_fat.hpp"
using controller = conctrl::concow_fat<tree::interface>;
#endif
struct request { uint64_t key,value,ticket=0; };
int main(int argc,char** argv) {
  if(argc!=5) return 2;
  const unsigned workers=std::stoul(argv[1]);
  const bool inserts=std::string(argv[2])=="insert";
  const uint64_t records=std::stoull(argv[3]), updates=std::stoull(argv[4]);
  if(!workers || !records || !updates || updates>UINT32_MAX) return 2;
  std::vector<kv> data(records);
  for(uint64_t i=0;i<records;++i)data[i]={i,i+1000};
  std::vector<request> requests(updates);
  std::mt19937_64 random(182);
  for(uint64_t i=0;i<updates;++i)requests[i]={inserts?records+i:random()%records,i+1,0};
  if(inserts)std::shuffle(requests.begin(),requests.end(),random);
  std::vector<std::pair<uint64_t,uint64_t>> expected(records+(inserts?updates:0));
  for(uint64_t i=0;i<records;++i)expected[i]={0,i+1000};
  #if defined(TREE_BTREE) || defined(TREE_ART) || defined(TREE_AERT)
  auto root=tree::build(records,data.data());
#else
  auto root=tree::build(records,2,data.data());
#endif
#ifdef ORIGINAL_TREE
#if defined(TREE_BTREE)
  auto s=std::make_unique<controller>(workers,root);
#else
  auto s=std::make_unique<controller>(3,workers,root);
#endif
#else
  auto s=std::make_unique<controller>(root,controller::options{
    .workers=workers,.readers=1,.slots=2,.gc=false,.parallel_materialization=true});
#endif
  std::atomic_uint64_t cursor=0;
  const auto begin=std::chrono::steady_clock::now();
  std::vector<std::thread> writers;
  for(unsigned i=0;i<4;++i)writers.emplace_back([&]{
    for(;;){
      auto first=cursor.fetch_add(64);
      if(first>=updates)break;
      for(auto j=first;j<std::min(first+64,updates);++j)
        requests[j].ticket=s->update(requests[j].key,requests[j].value);
    }
  });
  for(auto& t:writers)t.join();
  s->wait_for_processing(updates);
  const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
  for(auto op:requests)if(expected[op.key].first<op.ticket)expected[op.key]={op.ticket,op.value};
  uint64_t mismatches=0,first_bad=UINT64_MAX;
#ifdef ORIGINAL_TREE
  s->query([&](auto p){
    for(uint64_t k=0;k<expected.size();++k)if(tree::find(p,k)!=expected[k].second){++mismatches;first_bad=std::min(first_bad,k);}
  });
#else
  s->query([&](auto p,uint64_t v){
    for(uint64_t k=0;k<expected.size();++k)if(tree::find(p,k,v)!=expected[k].second){++mismatches;first_bad=std::min(first_bad,k);}
  });
#endif
  rusage usage{};getrusage(RUSAGE_SELF,&usage);
  std::cout<<"{\"workers\":"<<workers<<",\"clients\":4,\"gc\":false,\"records\":"<<records
    <<",\"updates\":"<<updates<<",\"workload\":\""<<(inserts?"insert":"update")
    <<"\",\"seconds\":"<<seconds<<",\"peak_rss_bytes\":"<<uint64_t(usage.ru_maxrss)
#ifndef __APPLE__
    *1024
#endif

    <<",\"first_bad\":"<<first_bad<<",\"mismatches\":"<<mismatches<<"}"<<std::endl;
  // Original trees keep all historical allocations. Each measurement uses a
  // fresh process; destruction stops the schedulers, process exit releases nodes.
  return mismatches?3:0;
}
