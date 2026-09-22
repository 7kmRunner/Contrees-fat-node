#define main contrees_program_main
#include "src/main.cpp"
#undef main
#include <cassert>
struct ticket_gaps {
  std::atomic_uint64_t requests{0};uint64_t waited=0;
  uint64_t update(uint64_t,uint64_t){return 2*(requests.fetch_add(1)+1);}
  template<class F> void query(uint64_t,F&&){}
  void wait_for_processing(uint64_t ticket){waited=ticket;}
};
int main(){
  std::vector<tx_context> txs(1000);
  for(unsigned i=0;i<txs.size();++i)txs[i]={uint8_t(i%3?1:0),i,i};
  ticket_gaps s;multi_client_execute<btree::interface>(4,txs.size(),txs.data(),&s);
  if(s.requests!=666 || s.waited!=1332)return 1;
  std::puts("CLI waits for the highest actual ticket, including checkpoint gaps");
}
