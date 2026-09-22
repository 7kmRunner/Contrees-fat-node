#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "common.hpp"
#include "context.hpp"
#include "reclamation.hpp"

namespace conctrl {

template <typename T>
class alignas(128) seqcow {
private:
  static constexpr uint64_t BATCH_SIZE = LIBCONCTRL_BUFFER_SIZE/16;

  std::atomic_bool start_;
  std::atomic_bool stop_;

  std::thread* const worker_;
  const uint8_t fat_slots_;
  std::mutex fat_submission_mutex_;

  alignas(128) epoch_reclaimer<T> gc_;

  alignas(128) std::atomic_uint64_t n_submitted_;
  alignas(128) std::atomic_uint64_t n_committed_;

  alignas(128) std::atomic_uint64_t checkpoint_;

  static_assert(sizeof(typename T::context) == 128);
  static_assert(alignof(typename T::context) == 128);
  T::context ctxs_[BUFFER_SIZE];

  void init_task_(uint64_t tid, uint64_t key, uint64_t val) {
    const uint64_t pos = tid % BUFFER_SIZE;
    ctxs_[pos].op = T::operand::INIT;
    ctxs_[pos].flags = 0;
    ctxs_[pos].gc_enabled = gc_.enabled();
    ctxs_[pos].fat_slots = fat_slots_;
    ctxs_[pos].sno = static_cast<uint32_t>(tid);
    ctxs_[pos].key = key;
    ctxs_[pos].val = val;
  }

  void exec_task_(uint64_t tid) {
    auto ctx = &ctxs_[tid%BUFFER_SIZE];
    ctx->t_past = ctxs_[(tid-1)%BUFFER_SIZE].root;
    ctx->op = T::handlers[0](ctx);
    while (ctx->op != DONE) { ctx->op = T::handlers[ctx->op](ctx); }
  }

  void worker_main_() {
    while (start_.load(std::memory_order_acquire) == 0);
    bool stop = false;

    uint64_t n_submitted_local = 0;
    uint64_t n_committed_local = 0;

    while (!stop || n_committed_local < n_submitted_local) {
      stop = stop_.load(std::memory_order_acquire);
      n_submitted_local = n_submitted_.load(std::memory_order_acquire);

      uint64_t n_committed_next = n_committed_local;
      while (n_committed_next < n_submitted_local) {
        exec_task_(++n_committed_next);
        gc_.retire(n_committed_next,
                   &ctxs_[n_committed_next%BUFFER_SIZE]);
        if (n_committed_next > n_committed_local + BUFFER_SIZE / 2) { break; }
      }
      if (n_committed_next > n_committed_local) {
        n_committed_local = n_committed_next;
        gc_.publish(ctxs_[n_committed_local%BUFFER_SIZE].root,
                    n_committed_local);
        n_committed_.store(n_committed_local, std::memory_order_release);
        gc_.try_reclaim(n_committed_local); } }
  }

public:
  seqcow(T::nodeptr t_init, uint64_t n_readers = 1,
         bool gc_enabled = false, uint8_t fat_slots = 0) :
    start_(false), stop_(false),
    worker_(new std::thread),
    fat_slots_(fat_slots),
    gc_(t_init, n_readers, gc_enabled, fat_slots != 0),
    n_submitted_(0), n_committed_(0), checkpoint_(0)
  {
    memset((void*)ctxs_, 0, sizeof(ctxs_));
    for (uint i = 0; i < BUFFER_SIZE; i++) { ctxs_[i].op = T::operand::DONE; }

    ctxs_[0].sno = 0;
    ctxs_[0].root = t_init;

    new (worker_) std::thread(&seqcow::worker_main_, this);
    start_.store(true, std::memory_order_release);
  }

  ~seqcow() {
    stop_.store(true, std::memory_order_release);
    worker_->join();

    std::atomic_thread_fence(std::memory_order_seq_cst);

    gc_.try_reclaim(n_committed_.load(std::memory_order_acquire));

    delete worker_;
  }

  void wait_for_processing(uint64_t n) {
    while (n_committed_.load(std::memory_order_acquire) < n);
  }

  uint64_t update(uint64_t key, uint64_t val) {
    if (fat_slots_ != 0) {
      /*
       * The original SeqCow path reserves a sequence number by publishing
       * n_submitted_ before it fills the corresponding context.  A fast
       * worker can therefore observe a partially initialized fat update.
       * Fat mode has one logical writer, so serialize client registration and
       * release-publish the frontier only after the context is complete.
       * The fat_slots == 0 path below intentionally remains unchanged.
       */
      std::lock_guard lock(fat_submission_mutex_);
      const uint64_t tid =
        1 + n_submitted_.load(std::memory_order_relaxed);
      if (tid > UINT32_MAX) throw std::overflow_error("fat-node version exhausted");
      while (tid >=
             n_committed_.load(std::memory_order_acquire) + BUFFER_SIZE);
      init_task_(tid, key, val);
      n_submitted_.store(tid, std::memory_order_release);
      return tid;
    }

    uint64_t tid;

    tid = 1+n_submitted_.fetch_add(1, std::memory_order_acquire);
    while (tid >= n_committed_.load(std::memory_order_acquire) + BUFFER_SIZE);

    init_task_(tid, key, val);

    return tid;
  }

  template <typename F>
  auto query(uint64_t cid, F&& f) {
    auto guard = gc_.pin(cid);
    if constexpr (requires { f(guard.root(), guard.version()); }) {
      return f(guard.root(), guard.version());
    } else {
      /* A root-only callback cannot filter unpublished, higher-version slots. */
      if (fat_slots_ != 0) { std::abort(); }
      return f(guard.root());
    }
  }

  template <typename F>
  auto query(F&& f) {
    return query(0, std::forward<F>(f));
  }

  void collect_garbage() {
    gc_.try_reclaim(n_committed_.load(std::memory_order_acquire));
  }

  typename epoch_reclaimer<T>::statistics gc_statistics() const {
    return gc_.get_statistics();
  }
};

}
