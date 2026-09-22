#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include "lib/conctrl/concow_fat.hpp"
#include "lib/conctrl/seqcow.hpp"
#include "radix_walk.hpp"
#include "utils/memory_stats.hpp"
using batch_controller=conctrl::concow_fat<art::interface>;
struct request {uint64_t key,value,ticket=0;};
template<class S> art::nodeptr measure(S& s,std::vector<request>& requests,uint64_t records,bool pinned) {
  std::atomic_uint64_t cursor=0;std::vector<std::pair<uint64_t,uint64_t>> expected(records+requests.size(),{0,0});
  std::chrono::steady_clock::time_point begin,finish;
  auto execute=[&]{begin=std::chrono::steady_clock::now();std::vector<std::thread> clients;
    for(unsigned c=0;c<4;++c)clients.emplace_back([&]{for(;;){auto first=cursor.fetch_add(64);if(first>=requests.size())break;
      for(auto i=first;i<std::min<uint64_t>(first+64,requests.size());++i)requests[i].ticket=s.update(requests[i].key,requests[i].value);}});
    for(auto& t:clients)t.join();s.wait_for_processing(requests.size());finish=std::chrono::steady_clock::now();
  };
  if(pinned)s.query([&](auto old,uint64_t v){execute();
    auto values=art::scan(old,0,records+requests.size(),v);
    if(v || values.size()!=records)throw std::runtime_error("old snapshot size changed");
    for(uint64_t i=0;i<records;++i)if(values[i]!=kv(i,0))throw std::runtime_error("old snapshot changed");
  });else execute();
  auto elapsed=std::chrono::duration<double>(finish-begin).count();
  for(auto r:requests)if(expected[r.key].first<r.ticket)expected[r.key]={r.ticket,r.value};
  uint64_t live=0,overflow=0;art::nodeptr final_root;
  s.query([&](auto p,uint64_t v){
    if(v!=requests.size())throw std::runtime_error("incomplete prefix");
    auto actual=art::scan(p,0,expected.size(),v);size_t pos=0;
    for(size_t key=0;key<expected.size();++key)if(key<records || expected[key].first){
      if(pos>=actual.size() || actual[pos++]!=kv(key,expected[key].second))throw std::runtime_error("reference mismatch");
    }
    if(pos!=actual.size())throw std::runtime_error("unexpected key");
    final_root=const_cast<art::nodeptr>(p);
    radix_bench::walk(final_root,[&](auto node){live+=art::node_size(node);if(node->type==art::LEAF)overflow+=((art::leaf*)node)->fat.allocated_bytes();});
  });
  s.collect_garbage();auto gc=s.gc_statistics();if(gc.pending_bytes || gc.retired_bytes!=gc.reclaimed_bytes)throw std::runtime_error("GC pending");
  auto memory=contrees::memory_stats::sample_now();
  std::cout<<",\"elapsed_seconds\":"<<elapsed<<",\"peak_rss_bytes\":"<<memory.peak_rss_bytes
    <<",\"live_bytes\":"<<live<<",\"overflow_bytes\":"<<overflow<<",\"retired_bytes\":"<<gc.retired_bytes
    <<",\"reclaimed_bytes\":"<<gc.reclaimed_bytes<<",\"pending_bytes\":"<<gc.pending_bytes;
  if constexpr(requires {s.execution_statistics();}){auto stats=s.execution_statistics();
    if(stats.appended_updates+stats.cow_updates!=requests.size())throw std::runtime_error("update accounting");
    std::cout<<",\"appended_updates\":"<<stats.appended_updates<<",\"cow_updates\":"<<stats.cow_updates
      <<",\"parallel_updates\":"<<stats.parallel_updates<<",\"materialization_waves\":"<<stats.materialization_waves;
  }else std::cout<<",\"appended_updates\":null,\"cow_updates\":null,\"parallel_updates\":0,\"materialization_waves\":null";
  std::cout<<",\"reference_ok\":true}\n";return final_root;
}
int main(int argc,char** argv){try{
  if(argc!=8)throw std::invalid_argument("mode(seq|cow|batch) slots workers pattern(uniform|hot|insert) pinned records updates");
  std::string mode=argv[1],pattern=argv[4];auto slots=std::stoul(argv[2]),workers=std::stoul(argv[3]);
  bool pinned=std::stoul(argv[5]);uint64_t records=std::stoull(argv[6]),updates=std::stoull(argv[7]);
  if(!records || !updates || updates>UINT32_MAX || !workers || workers>64 ||
     (slots!=0 && slots!=2 && slots!=4 && slots!=8) || (mode!="seq" && mode!="cow" && mode!="batch") ||
     (pattern!="uniform" && pattern!="hot" && pattern!="insert") || (mode=="seq" && (!slots || workers!=1)))
    throw std::invalid_argument("invalid options; seq comparison requires enabled fat slots and one writer");
  std::vector<kv> data;for(uint64_t i=0;i<records;++i)data.emplace_back(i,0);
  std::mt19937_64 rng(182);std::vector<request> requests;
  for(uint64_t i=0;i<updates;++i){auto key=pattern=="insert"?records+i:pattern=="hot" && rng()%10<9?rng()%std::min<uint64_t>(records,100):rng()%records;requests.push_back({key,i+1});}
  if(pattern=="insert")std::shuffle(requests.begin(),requests.end(),rng);
  auto root=art::build(records,data.data());art::nodeptr final_root;
  std::cout<<"{\"mode\":\""<<mode<<"\",\"slots\":"<<slots<<",\"workers\":"<<workers
    <<",\"pattern\":\""<<pattern<<"\",\"pinned\":"<<pinned<<",\"records\":"<<records<<",\"updates\":"<<updates;
  if(mode=="seq"){
    auto s=std::make_unique<conctrl::seqcow<art::interface>>(root,1,true,uint8_t(slots));
    final_root=measure(*s,requests,records,pinned);
  }else{
    batch_controller s(root,{.workers=unsigned(workers),.readers=1,.slots=uint8_t(slots),.gc=true,.parallel_materialization=mode=="batch"});
    final_root=measure(s,requests,records,pinned);
  }
  radix_bench::walk(final_root,[](auto p){art::free_node(p);});
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
