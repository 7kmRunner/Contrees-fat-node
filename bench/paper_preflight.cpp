// External correctness check; original library files are never edited.
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include "src/config.hpp"
#include "src/adapters/btree.hpp"
#include "src/adapters/betree.hpp"
#include "src/adapters/art.hpp"
#include "src/adapters/aert.hpp"
#include "lib/conctrl/concow.hpp"
#include "lib/conctrl/concow_cyclic.hpp"
struct operation { uint8_t type; uint64_t key, value; };
static_assert(sizeof(operation)==24);
template<class T, class S>
int check(S& scheduler, const config& cfg, const std::vector<kv>& initial,
          const std::vector<operation>& ops) {
  std::atomic_uint64_t cursor{0}, last{0}, writes{0};
  std::vector<std::thread> clients;
  for(unsigned cid=0;cid<cfg.num_clients;++cid) clients.emplace_back([&,cid]{
    uint64_t local_last=0, count=0;
    for(;;) {
      auto first=cursor.fetch_add(64);
      if(first>=ops.size())break;
      for(auto j=first;j<std::min<uint64_t>(first+64,ops.size());++j) {
        const auto& op=ops[j];
        if(op.type==1){local_last=std::max(local_last,scheduler.update(op.key,op.value));++count;}
        else {
#ifdef PAPER_FAT
          scheduler.query(cid,[&](auto root,uint64_t version){
            if(op.value)T::scan(root,op.key,op.value,version);else T::find(root,op.key,version);
          });
#else
          scheduler.query([&](auto root){
            if(op.value)T::scan(root,op.key,op.value);else T::find(root,op.key);
          });
#endif
        }
      }
    }
    auto old=last.load();while(old<local_last && !last.compare_exchange_weak(old,local_last)){}
    writes.fetch_add(count);
  });
  for(auto& t:clients)t.join();
  scheduler.wait_for_processing(last.load());
  uint64_t errors=0;
  // Original exporter uses value=key, for both initial records and every write.
  auto verify=[&](auto find){
    for(const auto& item:initial)if(find(item.first)!=item.second)++errors;
    for(const auto& op:ops)if(op.type==1 && find(op.key)!=op.value)++errors;
  };
#ifdef PAPER_FAT
  scheduler.query(0,[&](auto root,uint64_t version){verify([&](uint64_t k){return T::find(root,k,version);});});
#else
  scheduler.query([&](auto root){verify([&](uint64_t k){return T::find(root,k);});});
#endif
  std::cout<<"{\"mismatches\":"<<errors<<",\"writes\":"<<writes.load()
           <<",\"last_ticket\":"<<last.load()<<",\"ticket_gap\":"
           <<(last.load()!=writes.load()?"true":"false")<<"}\n";
  return errors?3:0;
}
template<class T>int fixed(const config& c,std::vector<kv>& data,const std::vector<operation>& ops){
 auto root=T::build(c,data.size(),data.data());
#ifdef PAPER_FAT
 auto scheduler=std::make_unique<conctrl::concow<T>>(c.num_pipes,c.num_workers,root,c.num_clients,false,c.fat_slots);
#else
 auto scheduler=std::make_unique<conctrl::concow<T>>(c.num_pipes,c.num_workers,root);
#endif
 return check<T>(*scheduler,c,data,ops);
}
template<unsigned P>int cyclic(const config& c,std::vector<kv>& data,const std::vector<operation>& ops){
 using T=btree::interface;auto root=T::build(c,data.size(),data.data());
#ifdef PAPER_FAT
 auto scheduler=std::make_unique<conctrl::concow_cyclic<T,P>>(c.num_workers,root,c.num_clients,false,c.fat_slots,true,c.fat_workers);
#else
 auto scheduler=std::make_unique<conctrl::concow_cyclic<T,P>>(c.num_workers,root);
#endif
 return check<T>(*scheduler,c,data,ops);
}
int main(int argc,char** argv){
 auto c=parse_args(argc,argv);std::ifstream f(c.dataset,std::ios::binary);
 uint64_t n=0,m=0;f.read((char*)&n,8);f.read((char*)&m,8);
 if(!f || n>1000000 || m>1000000)return 2;
 std::vector<uint64_t> keys(n);std::vector<operation> ops(m);
 f.read((char*)keys.data(),n*8);f.read((char*)ops.data(),m*sizeof(operation));if(!f)return 2;
 std::vector<kv> data;data.reserve(n);for(auto k:keys)data.emplace_back(k,k);
 for(const auto& op:ops)if(op.type>1 || (op.type==1 && op.key!=op.value))return 2;
 switch(c.structure){
 case ST_BTREE:switch(c.num_pipes){
 case 1:return cyclic<1>(c,data,ops);case 2:return cyclic<2>(c,data,ops);
 case 3:return cyclic<3>(c,data,ops);case 4:return cyclic<4>(c,data,ops);
 case 5:return cyclic<5>(c,data,ops);default:return 2;}
 case ST_BETREE:return fixed<betree::interface>(c,data,ops);
 case ST_ART:return fixed<art::interface>(c,data,ops);
 case ST_AERT:return fixed<aert::interface>(c,data,ops);
 default:return 2;
 }
}
