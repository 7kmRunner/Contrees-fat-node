#pragma once
#include "src/adapters/art.hpp"
#include "src/adapters/aert.hpp"
namespace radix_bench {
template<class F> void walk(art::nodeptr t,F&& f) {
  if(!t)return;
  switch(t->type){
    case art::NODE4: for(unsigned i=0;i<t->size;++i)walk(((art::node4*)t)->chs[i],f);break;
    case art::NODE16: for(unsigned i=0;i<t->size;++i)walk(((art::node16*)t)->chs[i],f);break;
    case art::NODE48: for(unsigned i=0;i<t->size;++i)walk(((art::node48*)t)->chs[i],f);break;
    case art::NODE256: for(unsigned i=0;i<256;++i)walk(((art::node256*)t)->chs[i],f);break;
  }
  f(t);
}
template<class F> void walk(aert::nodeptr tagged,F&& f) {
  auto t=aert::extract_ptr(tagged);if(!t)return;
  switch(t->type){
    case aert::NODE4: for(unsigned i=0;i<t->size;++i)walk(((aert::node4*)t)->chs[i],f);break;
    case aert::NODE16: for(unsigned i=0;i<t->size;++i)walk(((aert::node16*)t)->chs[i],f);break;
    case aert::NODEH4: for(unsigned i=0;i<t->size;++i)walk(((aert::nodeh4*)t)->chs[i],f);break;
    case aert::NODEH16: for(unsigned i=0;i<16;++i)walk(((aert::nodeh16*)t)->chs[i],f);break;
  }
  f(t);
}
}
