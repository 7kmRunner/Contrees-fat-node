// Isolate materialization costs; source construction and reference checks are untimed.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include "lib/trees/btree/update_cow.hpp"
int main(int argc,char** argv) {
  if(argc!=5)return 2;
  const unsigned slots=std::stoul(argv[1]);
  const bool worker=std::string(argv[2])=="worker";
  const bool inserts=std::string(argv[3])=="insert";
  const unsigned iterations=std::stoul(argv[4]);
  if(!btree::supported_fat_slots(slots) || !iterations)return 2;
  auto source=btree::new_leaf(0,15);
  std::map<uint64_t,uint64_t> expected;
  for(unsigned i=0;i<15;++i){source->keys[i]=2*i;source->vals[i]=1000+i;expected[2*i]=1000+i;}
  for(unsigned i=0;i<slots;++i){
    auto key=inserts?2*i+1:0;
    if(!btree::append_leaf_delta(source,slots,i+1,key,2000+i))return 3;
    expected[key]=2000+i;
  }
  btree::context ctx{};ctx.sno=slots+1;ctx.key=inserts?2*slots+1:0;ctx.val=3000;ctx.fat_slots=slots;
  expected[ctx.key]=ctx.val;
  uint64_t checksum=0;
  auto run=[&](bool check){
    auto [separator,left,right]=worker?btree::prepare_leaf_cow(source,&ctx):btree::materialize_leaf_cow(source,&ctx);
    btree::nodeptr parent=nullptr;
    if(worker){
      if(right)parent=btree::new_root(ctx.sno,separator,left,right);
      ctx.root=parent?parent:left;ctx.t_cur=parent;ctx.t_past=source;ctx.split_idx=0;ctx.leaf_pair=right!=nullptr;
      btree::finish_leaf_cow(&ctx);
    }
    if(check){
      auto it=expected.begin();
      for(auto leaf:{left,right})if(leaf)for(unsigned i=0;i<leaf->size;++i){
        if(it==expected.end() || it->first!=leaf->keys[i] || it->second!=leaf->vals[i])std::abort();
        ++it;
      }
      if(it!=expected.end())std::abort();
    }
    checksum+=left->vals[0]+left->size+(right?right->vals[0]+right->size:0);
    btree::free_node(left);if(right)btree::free_node(right);if(parent)btree::free_node(parent);
  };
  run(true);
  const auto start=std::chrono::steady_clock::now();
  for(unsigned i=0;i<iterations;++i)run(false);
  const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
  btree::free_node(source);
  std::printf("{\"slots\":%u,\"worker\":%u,\"inserts\":%u,\"iterations\":%u,\"seconds\":%.9f,\"checksum\":%llu,\"reference_ok\":true}\n",slots,worker,inserts,iterations,seconds,(unsigned long long)checksum);
}
