// External verification driver; compile only against the archived original headers.
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
#include "src/adapters/btree.hpp"
#include "lib/conctrl/concow_cyclic.hpp"

#ifndef ORIGINAL_PIPES
#define ORIGINAL_PIPES 3
#endif
using controller = conctrl::concow_cyclic<btree::interface, ORIGINAL_PIPES>;
struct request { uint64_t key,value,ticket=0; };
int main(int argc,char** argv) {
  if(argc!=6) return 2;
  const unsigned workers=std::stoul(argv[1]);
  const unsigned clients=std::stoul(argv[5]);
  const bool hot=std::string(argv[2])=="hot";
  const bool inserts=std::string(argv[2])=="insert";
  const uint64_t records=std::stoull(argv[3]), updates=std::stoull(argv[4]);
  if(!workers || !records || !updates || updates>UINT32_MAX) return 2;
  std::vector<kv> data(records);
  for(uint64_t i=0;i<records;++i)data[i]={i,i+1000};
  std::vector<request> requests(updates);
  std::mt19937_64 random(182);
  for(uint64_t i=0;i<updates;++i)requests[i]={inserts?records+i:(hot && random()%10<9?random()%std::min<uint64_t>(100,records):random()%records),i+1,0};
  if(inserts)std::shuffle(requests.begin(),requests.end(),random);
  std::vector<std::pair<uint64_t,uint64_t>> expected(records+(inserts?updates:0));
  for(uint64_t i=0;i<records;++i)expected[i]={0,i+1000};
  auto root=btree::build(records,data.data());
  auto s=std::make_unique<controller>(workers,root);
  std::atomic_uint64_t cursor=0;
  const auto begin=std::chrono::steady_clock::now();
  std::vector<std::thread> writers;
  for(unsigned i=0;i<clients;++i)writers.emplace_back([&]{
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
  uint64_t mismatches=0,first_bad=UINT64_MAX,live_bytes=0;
  auto inspect=[&](auto&& self,btree::cnodeptr p)->void {
    live_bytes+=sizeof(btree::node);
    if(p->type!=btree::LEAF)for(unsigned i=0;i<=p->size;++i)self(self,p->chs[i]);
  };
  s->query([&](auto p){
    for(uint64_t k=0;k<expected.size();++k)if(btree::find(p,k)!=expected[k].second){++mismatches;first_bad=std::min(first_bad,k);}
    inspect(inspect,p);
  });
  rusage usage{};getrusage(RUSAGE_SELF,&usage);
  std::cout<<"{\"workers\":"<<workers<<",\"clients\":"<<clients<<",\"pipes\":"<<ORIGINAL_PIPES<<",\"gc\":false,\"records\":"<<records
    <<",\"updates\":"<<updates<<",\"workload\":\""<<(inserts?"insert":hot?"hot":"update")
    <<"\",\"seconds\":"<<seconds<<",\"peak_rss_bytes\":"<<usage.ru_maxrss
    <<",\"live_bytes\":"<<live_bytes<<",\"mismatches\":"<<mismatches<<"}"<<std::endl;
  // Original trees keep all historical allocations. Each measurement uses a
  // fresh process; destruction stops the schedulers, process exit releases nodes.
  return mismatches?3:0;
}
