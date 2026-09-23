#pragma once
// Diagnostic-only timer wrapper. The underlying tree/scheduler sources are untouched.
#include "utils/timer.hpp"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
struct profile_timer {
  timer inner;
  unsigned starts=0;
  int ctl=-1, ack=-1;
  bool enabled=false;
  void command(const char* message) {
    if(ctl<0) {
      const char* c=std::getenv("CONTREES_PERF_CTL");
      const char* a=std::getenv("CONTREES_PERF_ACK");
      if(!c || !a)throw std::runtime_error("missing profiling FIFO paths");
      ctl=open(c,O_RDWR|O_NONBLOCK);ack=open(a,O_RDWR|O_NONBLOCK);
      if(ctl<0 || ack<0)throw std::runtime_error("cannot open profiling FIFOs");
    }
    if(write(ctl,message,std::strlen(message))!=static_cast<ssize_t>(std::strlen(message)))
      throw std::runtime_error("perf control write failed");
    std::string response;
    while(response.find('\n')==std::string::npos) {
      pollfd fd{ack,POLLIN,0};
      if(poll(&fd,1,10000)<=0)throw std::runtime_error("perf acknowledgement timed out");
      char buffer[32];auto n=read(ack,buffer,sizeof(buffer));
      if(n<=0)throw std::runtime_error("perf acknowledgement read failed");
      response.append(buffer,static_cast<size_t>(n));
      if(response.size()>64)throw std::runtime_error("unexpected perf acknowledgement");
    }
    if(response!="ack\n")throw std::runtime_error("unexpected perf acknowledgement");
    fprintf(stderr,"[PROFILE] %s",message);
  }
  void start() {
    if(++starts==2){command("enable\n");enabled=true;}
    inner.start();
  }
  double count() {
    double seconds=inner.count();
    if(enabled){command("disable\n");enabled=false;}
    return seconds;
  }
  ~profile_timer(){if(ctl>=0)close(ctl);if(ack>=0)close(ack);}
};
