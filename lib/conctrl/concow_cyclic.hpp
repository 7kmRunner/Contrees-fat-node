#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <stdexcept>

#include "common.hpp"
#include "mpsc_list.hpp"
#include "reclamation.hpp"

namespace conctrl {

template <typename T, uint P>
class alignas(128) concow_cyclic {
private:
  static constexpr uint N_PIPES = P+1;
  static constexpr uint64_t BATCH_ROUNDS = LIBCONCTRL_BUFFER_SIZE/32;
  static constexpr uint64_t BATCH_SIZE = BATCH_ROUNDS*N_PIPES;
  static_assert(std::atomic_uint64_t::is_always_lock_free);
  static_assert(std::atomic<typename T::nodeptr>::is_always_lock_free);

  std::atomic_bool start_;
  std::atomic_bool stop_;

  alignas(uint64_t) uint8_t _flags_[0];

  const uint n_workers_;
  const uint8_t fat_slots_;
  const bool local_fat_probe_;
  const bool worker_fat_;
  // Only entry stages access this state, ordered by the existing cyclic
  // handoff: entry(t+1) follows its inner/exit step for t, which acquired the
  // pace published by entry(t). No additional entry spin gate is needed.
  // The global
  // gate remains available as a comparison; the local probe checks completed
  // nodes on its own path. Neither probe waits for downstream work.
  uint64_t last_structural_ = 0;
  std::atomic_uint64_t fat_appends_{0}, structural_updates_{0}, blocked_probes_{0};

  static uint validate_workers(uint n) {
    if (!n) throw std::invalid_argument("ConCow requires a worker");
    return n;
  }
  static uint8_t validate_slots(uint8_t slots) {
    if (slots!=0 && slots!=2 && slots!=4 && slots!=8)
      throw std::invalid_argument("invalid fat slots");
    if constexpr (!requires(typename T::context* c) { T::pipeline_fat_begin(c, true); }) {
      if(slots) throw std::invalid_argument("tree has no native pipeline fat adapter");
    }
    return slots;
  }
  static bool validate_worker_fat(bool enabled,uint8_t slots,bool local_probe) {
    if(!enabled || !slots)return false;
    if constexpr(!requires(typename T::context* ctx) {
      T::pipeline_fat_reserve(ctx,uint64_t{});
      T::pipeline_fat_try(ctx,uint64_t{});
      T::pipeline_fat_configure(ctx,true);
      T::pipeline_fat_task(ctx);
    })throw std::invalid_argument("tree has no worker fat adapter");
    if(!local_probe)throw std::invalid_argument("worker fat requires local probe");
    return true;
  }

  std::thread* const monitor_;

  struct alignas(128) pipe_context {
    std::thread p;
    volatile uint64_t pace = 0;
    void wait(uint64_t* cache, uint64_t expect) { while (*cache < expect) { *cache = load_consume(&pace); } }
    void advance(uint64_t next) { store_release(&pace, next); }
  } *const pipes_;

  struct alignas(128) worker_context {
    std::thread p;
    std::atomic_uint64_t tid = 0;
    worker_context* volatile next = nullptr;
    std::atomic_uint64_t fat_appends{0}, fat_materializations{0};
  } *const workers_;

  alignas(128) mpsc_list<worker_context> workers_available_;

  // The monitor exclusively owns retirement and reclamation. External
  // collectors request a pass; they never access the retirement queue.
  alignas(128) epoch_reclaimer<T, true> gc_;
  alignas(128) std::atomic_uint64_t gc_requested_{0};
  std::atomic_uint64_t gc_completed_{0};

  alignas(128) std::atomic_uint64_t n_submitted_;
  alignas(128) std::atomic_uint64_t n_inited_;
  alignas(128) std::atomic_uint64_t n_committed_;

  struct alignas(128) {
    volatile uint64_t stage;
    uint64_t load() { return load_consume(&stage); }
    void store(uint64_t next) { store_release(&stage, next); }
  } stages_[BUFFER_SIZE];

  static_assert(alignof(typename T::context) == 128);
  T::context ctxs_[BUFFER_SIZE];

  static int worker_fat_task(typename T::context* ctx) {
    if constexpr (requires { T::pipeline_fat_task(ctx); })return T::pipeline_fat_task(ctx);
    return 0;
  }

  inline void wait_stage(T::context* ctx, uint64_t stage) {
    uint32_t ver = ctx->t_past->ver;
    while (ctx->cno < ver) {
      uint64_t pstage = stages_[ver%BUFFER_SIZE].load();
      if (pstage >= stage) { break; }
      ctx->cno = (uint32_t)n_committed_.load(std::memory_order_acquire); }
  }

  template <uint PID>
  void process_entry(uint64_t tid, uint64_t* n_tasks) {
    while (*n_tasks < tid) { *n_tasks = n_inited_.load(std::memory_order_acquire); }
    auto ctx = &ctxs_[tid%BUFFER_SIZE];
    ctx->t_past = ctxs_[(tid-1)%BUFFER_SIZE].root;
    if (ctx->op != 0xf) [[likely]] {
      if constexpr (requires { T::pipeline_fat_begin(ctx, true); }) {
        if(fat_slots_) {
          auto previous_root=ctx->t_past;
          const auto committed=n_committed_.load(std::memory_order_acquire);
          if constexpr (requires { T::pipeline_fat_try(ctx, committed); }) {
            if (worker_fat_) {
              if constexpr (requires { T::pipeline_fat_reserve(ctx,committed); }) {
                const int result=T::pipeline_fat_reserve(ctx,committed);
                if(result<0)blocked_probes_.fetch_add(1,std::memory_order_relaxed);
                if(result<=0)ctx->op=T::pipeline_fat_begin(ctx,false);
              }
            } else if (local_fat_probe_) {
              const int result=T::pipeline_fat_try(ctx,committed);
              if(result<0) blocked_probes_.fetch_add(1,std::memory_order_relaxed);
              ctx->op=result>0 ? T::operand::DONE : T::pipeline_fat_begin(ctx,false);
            } else {
              const bool ready=committed>=last_structural_;
              if(!ready) blocked_probes_.fetch_add(1,std::memory_order_relaxed);
              ctx->op=T::pipeline_fat_begin(ctx,ready);
            }
          } else {
            const bool ready=committed>=last_structural_;
            if(!ready) blocked_probes_.fetch_add(1,std::memory_order_relaxed);
            ctx->op=T::pipeline_fat_begin(ctx,ready);
          }
          if(ctx->root==previous_root) fat_appends_.fetch_add(1,std::memory_order_relaxed);
          else {
            last_structural_=tid;
            structural_updates_.fetch_add(1,std::memory_order_relaxed);
          }
        } else ctx->op=T::handlers[ctx->op](ctx);
      } else ctx->op=T::handlers[ctx->op](ctx);
    }
    pipes_[(PID+1)%N_PIPES].advance(tid);
  }

  template <uint PID>
  void process_inner(uint64_t tid, uint64_t* pred_pace) {
    pipes_[PID].wait(pred_pace, tid);
    auto ctx = &ctxs_[tid%BUFFER_SIZE];
    if (ctx->op != 0xf && !worker_fat_task(ctx)) [[likely]] { ctx->op = T::handlers[ctx->op](ctx); }
    pipes_[(PID+1)%N_PIPES].advance(tid);
  }

  template <uint PID>
  void process_exit(uint64_t tid, uint64_t* pred_pace, uint64_t* n_compl) {
    pipes_[PID].wait(pred_pace, tid);
    auto ctx = &ctxs_[tid%BUFFER_SIZE];
    if (ctx->op != 0xf) [[likely]] {
      ctx->cno = *n_compl;
      if(!worker_fat_task(ctx)) {
        wait_stage(ctx, 1);
        ctx->op = T::handlers[ctx->op](ctx);
      }
      *n_compl = ctx->cno;
      worker_context* wctx = workers_available_.pop();
      while (wctx == nullptr) { wctx = workers_available_.pop(); }
      wctx->tid.store(tid, std::memory_order_release); }
    else { stages_[tid%BUFFER_SIZE].store(~0UL); }
  }

  template <uint PID, uint TID>
  inline void process_step(uint64_t tbase, uint64_t* n_tasks, uint64_t* pred_pace, uint64_t* n_compl) {
    if constexpr (PID == TID) { process_entry<PID>(tbase+TID, n_tasks); }
    else if constexpr ((PID+1)%N_PIPES == TID) { process_exit<PID>(tbase+TID, pred_pace, n_compl); }
    else { process_inner<PID>(tbase+TID, pred_pace); }
  }

  template <uint PID, uint TID>
  void pipe_subloop_(uint64_t tbase, uint64_t* n_tasks, uint64_t* pred_pace, uint64_t* n_compl) {
    if constexpr (PID == TID) { process_entry<PID>(tbase+TID, n_tasks); }
    else if constexpr ((PID+1)%N_PIPES == TID) { process_exit<PID>(tbase+TID, pred_pace, n_compl); }
    else { process_inner<PID>(tbase+TID, pred_pace); }

    if constexpr (TID+1 < N_PIPES) { pipe_subloop_<PID, TID+1>(tbase, n_tasks, pred_pace, n_compl); }
  }

  template <uint PID>
  void pipe_iter_(uint64_t first, uint64_t last) {
    alignas(128) static thread_local struct {
      uint64_t n_tasks = 0;
      uint64_t pred_pace = 0;
      uint64_t n_compl = 0;
    } cache;
    for (uint64_t tid = first+1; tid <= last; tid += N_PIPES) {
      pipe_subloop_<PID, 0>(tid, &cache.n_tasks, &cache.pred_pace, &cache.n_compl); }
  }

  template <uint PID>
  void pipe_main_() {
    while (start_.load(std::memory_order_acquire) == 0);

    uint64_t local_epoch = 0;
    while (!stop_.load(std::memory_order_acquire)) {
      pipe_iter_<PID>(local_epoch*BATCH_SIZE, (local_epoch+1)*BATCH_SIZE);
      local_epoch++; }

    uint64_t n_tasks = n_submitted_.load(std::memory_order_acquire);
    pipe_iter_<PID>(local_epoch*BATCH_SIZE, n_tasks);
  }

  void worker_main_(uint wid) {
    while (start_.load(std::memory_order_acquire) == 0);

    worker_context* wctx = &workers_[wid-1];

    uint64_t last_tid = 0;
    while (true) {
      uint64_t tid;
      while ((tid = wctx->tid.load(std::memory_order_acquire)) == last_tid);
      if (tid == ~0UL) { break; }

      auto ctx = &ctxs_[tid%BUFFER_SIZE];
      for (uint64_t local_stage = 1; ctx->op != 0xf; local_stage++) {
        /* assert(ctx->op != 0x0; */
        const int task=worker_fat_task(ctx);
        if(!task)wait_stage(ctx, local_stage+1);
        if constexpr(requires { T::before_worker_step(ctx); })T::before_worker_step(ctx);
        ctx->op = T::handlers[ctx->op](ctx);
        if(task==1)wctx->fat_appends.store(wctx->fat_appends.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
        if(task==2)wctx->fat_materializations.store(wctx->fat_materializations.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
        stages_[(ctx->sno)%BUFFER_SIZE].store(local_stage); }
      stages_[(ctx->sno)%BUFFER_SIZE].store(~0UL);

      last_tid = tid;
      workers_available_.push(wctx); }
  }

  void monitor_main_() {
    while (start_.load(std::memory_order_acquire) == 0);
    bool stop = false;

    uint64_t n_submitted_local = 0;
    uint64_t n_inited_local = 0;
    uint64_t n_committed_local = 0;
    uint64_t last_reclaimed_frontier = 0;

    while (!stop || n_committed_local < n_submitted_local) {
      if constexpr (requires { T::on_monitor_pass(); }) T::on_monitor_pass();
      stop = stop_.load(std::memory_order_acquire);
      n_submitted_local = n_submitted_.load(std::memory_order_acquire);

      /* sync contexts from clients */
      uint64_t n_inited_next = n_inited_local;
      while (n_inited_next < n_submitted_local) {
        if (stages_[(n_inited_next+1)%BUFFER_SIZE].load()) { break; }
        ++n_inited_next; }
      if (n_inited_next > n_inited_local) {
        n_inited_local = n_inited_next;
        n_inited_.store(n_inited_local, std::memory_order_release); }

      /* sync outputs of workers */
      uint64_t n_committed_next = n_committed_local;
      while (n_committed_next < n_inited_local) {
          if (~stages_[(n_committed_next+1)%BUFFER_SIZE].load()) { break; }
          ++n_committed_next;
          gc_.retire(n_committed_next,
                     &ctxs_[n_committed_next%BUFFER_SIZE]); }
      if (n_committed_next > n_committed_local) {
        n_committed_local = n_committed_next;
        gc_.publish(ctxs_[n_committed_local%BUFFER_SIZE].root,
                    n_committed_local);
        n_committed_.store(n_committed_local, std::memory_order_release);
        /* The continuous committed prefix is ConTree's writer-safe frontier. */
      }
      // Acquire the request BEFORE scanning reader slots: unpin followed by
      // collect_garbage() must be reflected in the acknowledged collection.
      const auto requested=gc_requested_.load(std::memory_order_acquire);
      if (requested > gc_completed_.load(std::memory_order_relaxed) ||
          n_committed_local > last_reclaimed_frontier) {
        gc_.try_reclaim(n_committed_local);
        last_reclaimed_frontier=n_committed_local;
        gc_completed_.store(requested,std::memory_order_release);
      }
    }

    /* append bubbles to terminate the pipeline */
    uint64_t margin = BATCH_SIZE + (BATCH_SIZE - (n_inited_local%BATCH_SIZE)) % BATCH_SIZE;
    n_inited_.store(n_inited_local+margin+N_PIPES, std::memory_order_release);
  }

  template <uint I>
  void create_pipes_() {
    new (&pipes_[I].p) std::thread(&concow_cyclic::pipe_main_<I>, this);
    if constexpr (I+1 < N_PIPES) { create_pipes_<I+1>(); }
  }

public:
  concow_cyclic(uint n_workers, T::nodeptr t_init,
                uint64_t n_readers = 1, bool gc_enabled = false, uint8_t fat_slots = 0,
                bool local_fat_probe = true, bool worker_fat = false) :
    start_(false), stop_(false), n_workers_(validate_workers(n_workers)),
    fat_slots_(validate_slots(fat_slots)),
    local_fat_probe_(local_fat_probe),
    worker_fat_(validate_worker_fat(worker_fat,fat_slots_,local_fat_probe_)),
    monitor_(new std::thread),
    pipes_(new pipe_context[N_PIPES]),
    workers_(new worker_context[n_workers]),
    workers_available_(),
    gc_(t_init, n_readers, gc_enabled, fat_slots_!=0),
    n_submitted_(0), n_inited_(0), n_committed_(0)
  {
    memset((void*)ctxs_, 0, sizeof(ctxs_));
    for (uint i = 0; i < BUFFER_SIZE; i++) {
      stages_[i].store(~0UL);
      ctxs_[i].op = T::operand::DONE; }

    ctxs_[0].sno = 0;
    ctxs_[0].root = t_init;

    new (monitor_) std::thread(&concow_cyclic::monitor_main_, this);
    create_pipes_<0>();
    for (uint i = 0; i < n_workers; i++) {
      new (&(workers_+i)->p) std::thread(&concow_cyclic::worker_main_, this, i+1);
      workers_available_.push(workers_+i); }

    start_.store(true, std::memory_order_release);
  }

  ~concow_cyclic() {
    stop_.store(true, std::memory_order_release);
    monitor_->join();

    std::atomic_thread_fence(std::memory_order_seq_cst);

    for (uint i = 0; i < N_PIPES; i++) { pipes_[i].p.join(); }

    std::atomic_thread_fence(std::memory_order_seq_cst);

    for (uint i = 0; i < n_workers_; i++) { workers_[i].tid.store(~0UL, std::memory_order_release); }
    for (uint i = 0; i < n_workers_; i++) { workers_[i].p.join(); }

    std::atomic_thread_fence(std::memory_order_seq_cst);

    gc_.try_reclaim(n_committed_.load(std::memory_order_acquire));

    delete monitor_;
    delete []pipes_;
    delete []workers_;
  }

  void wait_for_processing(uint64_t n) {
    while (n_committed_.load(std::memory_order_acquire) < n);
  }

  uint64_t update(uint64_t key, uint64_t val) {
    uint64_t prior=n_submitted_.load(std::memory_order_relaxed);
    do {
      if(prior==UINT32_MAX) throw std::overflow_error("ConCow version exhausted");
    } while(!n_submitted_.compare_exchange_weak(prior,prior+1,std::memory_order_acq_rel));
    uint64_t tid=prior+1;
    if constexpr (requires { T::on_submission_reserved(tid); })
      T::on_submission_reserved(tid);

    // simply spin if the buffer is full
    while (tid >= n_committed_.load(std::memory_order_acquire) + BUFFER_SIZE);

    uint64_t pos = tid % BUFFER_SIZE;
    ctxs_[pos].op = T::operand::INIT;
    ctxs_[pos].gc_enabled = gc_.enabled();
    ctxs_[pos].fat_slots = fat_slots_;
    ctxs_[pos].sno = (uint32_t)tid;
    ctxs_[pos].key = key;
    ctxs_[pos].val = val;
    if constexpr(requires { T::pipeline_fat_configure(&ctxs_[pos],worker_fat_); })
      T::pipeline_fat_configure(&ctxs_[pos],worker_fat_);
    stages_[pos].store(0); // to indicate the context is inited

    return tid;
  }

  template <typename F>
  auto query(uint64_t cid, F&& f) {
    auto guard = gc_.pin(cid);
    if constexpr (requires { f(guard.root(), guard.version()); }) {
      return f(guard.root(), guard.version());
    } else {
      if(fat_slots_) throw std::invalid_argument("fat query requires snapshot version");
      return f(guard.root());
    }
  }

  template <typename F>
  auto query(F&& f) {
    return query(0, std::forward<F>(f));
  }

  void collect_garbage() {
    if (!gc_.enabled()) return;
    const auto request=gc_requested_.fetch_add(1,std::memory_order_acq_rel)+1;
    if constexpr (requires { T::on_gc_requested(request); })
      T::on_gc_requested(request);
    while(gc_completed_.load(std::memory_order_acquire)<request)
      std::this_thread::yield();
  }

  typename epoch_reclaimer<T, true>::statistics gc_statistics() const {
    return gc_.get_statistics();
  }
  struct fat_statistics { uint64_t appended, structural, blocked_probes, worker_appends, worker_materializations; };
  fat_statistics fat_execution_statistics() const {
    uint64_t appended=0,materialized=0;
    for(unsigned i=0;i<n_workers_;++i) {
      appended+=workers_[i].fat_appends.load(std::memory_order_relaxed);
      materialized+=workers_[i].fat_materializations.load(std::memory_order_relaxed);
    }
    return {fat_appends_.load(),structural_updates_.load(),blocked_probes_.load(),appended,materialized};
  }
  // Individual counters, not an atomic snapshot. Useful for diagnosing an
  // admission hole versus a handler/commit dependency.
  struct progress_statistics { uint64_t submitted, initialized, committed; };
  progress_statistics progress() const {
    return {n_submitted_.load(std::memory_order_acquire),
            n_inited_.load(std::memory_order_acquire),
            n_committed_.load(std::memory_order_acquire)};
  }
};

}
