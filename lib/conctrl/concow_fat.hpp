#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "reclamation.hpp"

namespace conctrl {

// Experimental ordered-wave controller. Unlike the legacy pipelined ConCow,
// the root is immutable throughout an append wave. A group owns one leaf and
// is executed by one worker; distinct groups may run concurrently. Structural
// COW is an exclusive coordinator phase. Only adapters explicitly providing
// concurrent_fat_* hooks can instantiate this controller (B+Tree, ART, AERT,
// and BeTree).
// This is NOT a drop-in enablement of legacy ConCow's fat_slots option.
template<class T, bool Profile = false>
class concow_fat {
public:
  struct options {
    unsigned workers = 2;
    uint64_t readers = 1;
    uint8_t slots = 2;
    bool gc = true;
    size_t queue_capacity = 4096;
    size_t batch_size = 256;
    size_t parallel_min_groups = 16;
    bool parallel_materialization = false;
  };
  struct statistics {
    uint64_t appended_updates;
    uint64_t cow_updates;
    uint64_t append_waves;
    uint64_t max_wave_groups;
    uint64_t parallel_waves;
    uint64_t parallel_updates;
    uint64_t materialization_waves;
    uint64_t parallel_materializations;
  };
  struct phase_statistics {
    uint64_t queue_ns, plan_ns, inline_leaf_ns, parallel_wave_ns;
    uint64_t merge_retire_ns, publish_ns, gc_ns, serial_cow_ns;
    uint64_t worker_loop_ns, worker_wakeups;
  };

private:
  using nodeptr = typename T::nodeptr;
  struct operation { uint64_t ticket, key, value; };
  struct task { operation op; size_t next; };
  struct group {
    nodeptr leaf; size_t first, last, count;
    bool materialize = false;
    typename T::concurrent_fat_replacement replacement{};
  };
  // A null target denotes a missing key (radix insertion). Those reservations
  // are grouped by key, whereas existing targets are grouped by physical leaf.
  struct bucket { nodeptr leaf = nullptr; size_t group_index = 0; uint64_t key = 0; bool occupied = false; };
  static constexpr size_t END = std::numeric_limits<size_t>::max();

  const options opts_;
  epoch_reclaimer<T> gc_;
  nodeptr root_; // coordinator only; readers use gc_.pin()
  std::vector<operation> queue_;
  std::vector<task> batch_;
  std::vector<group> groups_;
  std::vector<bucket> table_;

  std::mutex input_mutex_;
  std::condition_variable input_cv_, commit_cv_;
  uint64_t submitted_ = 0; // all input state protected by input_mutex_
  bool stopping_ = false;
  std::exception_ptr failure_;
  std::atomic_uint64_t committed_ {0};

  std::mutex pool_mutex_;
  std::condition_variable work_cv_, done_cv_;
  uint64_t generation_ = 0;
  size_t remaining_ = 0;
  bool pool_stopping_ = false;
  std::exception_ptr worker_failure_;
  std::atomic_size_t next_group_ {0};
  std::vector<std::thread> workers_;
  std::thread coordinator_;

  std::atomic_uint64_t appended_ {0}, cows_ {0}, waves_ {0}, max_groups_ {0};
  std::atomic_uint64_t parallel_waves_ {0}, parallel_updates_ {0};
  std::atomic_uint64_t materialization_waves_ {0}, parallel_materializations_ {0};
  std::atomic_uint64_t queue_ns_{0}, plan_ns_{0}, inline_leaf_ns_{0}, parallel_wave_ns_{0};
  std::atomic_uint64_t merge_retire_ns_{0}, publish_ns_{0}, gc_ns_{0}, serial_cow_ns_{0};
  std::atomic_uint64_t worker_loop_ns_{0}, worker_wakeups_{0};
  // Profile=false compiles out clock reads and counter updates. Worker time
  // is summed across threads and overlaps coordinator parallel_wave_ns.
  struct phase_timer {
    std::atomic_uint64_t& counter;
    std::chrono::steady_clock::time_point start;
    explicit phase_timer(std::atomic_uint64_t& c) : counter(c) {
      if constexpr(Profile) start=std::chrono::steady_clock::now();
    }
    ~phase_timer() {
      if constexpr(Profile) counter.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count(),std::memory_order_relaxed);
    }
  };

  static options validate(options opts) {
    if (!opts.workers || !opts.readers || !opts.queue_capacity ||
        !opts.batch_size || !opts.parallel_min_groups || opts.batch_size > opts.queue_capacity ||
        opts.batch_size > (1u << 20) ||
        (opts.slots != 0 && opts.slots != 2 && opts.slots != 4 && opts.slots != 8)) {
      throw std::invalid_argument("invalid experimental ConCow options");
    }
    return opts;
  }

  void execute_group(size_t index) {
    auto& g = groups_[index];
    for (size_t pos = g.first; pos != END; pos = batch_[pos].next) {
      const auto& op = batch_[pos].op;
      if (g.materialize && pos == g.last) {
        g.replacement = T::concurrent_fat_prepare(g.leaf, op.ticket, op.key, op.value);
        continue;
      }
      if (!T::concurrent_fat_append(g.leaf, opts_.slots, op.ticket, op.key, op.value)) {
        throw std::logic_error("reserved fat-leaf capacity was lost");
      }
    }
  }

  void worker_main() {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock lock(pool_mutex_);
        work_cv_.wait(lock, [&] { return pool_stopping_ || generation_ != seen; });
        if (pool_stopping_) return;
        seen = generation_;
      }
      try {
        phase_timer timer(worker_loop_ns_);
        if constexpr(Profile) worker_wakeups_.fetch_add(1,std::memory_order_relaxed);
        for (;;) {
          const size_t index = next_group_.fetch_add(1, std::memory_order_relaxed);
          if (index >= groups_.size()) break;
          execute_group(index);
        }
      } catch (...) {
        std::lock_guard lock(pool_mutex_);
        if (!worker_failure_) worker_failure_ = std::current_exception();
      }
      {
        std::lock_guard lock(pool_mutex_);
        if (--remaining_ == 0) done_cv_.notify_one();
      }
    }
  }

  // Called only while workers are quiescent. The fixed-size hash table tracks
  // per-leaf reservations for this wave, not persistent per-node metadata.
  size_t plan_wave(size_t first) {
    phase_timer timer(plan_ns_);
    groups_.clear();
    std::fill(table_.begin(), table_.end(), bucket{});
    size_t pos = first;
    for (; pos < batch_.size(); ++pos) {
      auto& item = batch_[pos];
      auto leaf = T::concurrent_fat_target(root_, item.op.key);
      const size_t available = T::concurrent_fat_available(leaf, opts_.slots);
      size_t hash = ((leaf ? (reinterpret_cast<uintptr_t>(leaf) >> 6) : item.op.key) * UINT64_C(11400714819323198485)) & (table_.size()-1);
      while (table_[hash].occupied && (table_[hash].leaf != leaf || (!leaf && table_[hash].key != item.op.key))) hash = (hash+1)&(table_.size()-1);
      auto& entry = table_[hash];
      const size_t reserved = entry.occupied ? groups_[entry.group_index].count : 0;
      if (reserved >= available + size_t(opts_.parallel_materialization)) break;
      // One materialization per original leaf per wave; subsequent operations
      // on that leaf wait for the newly merged routing structure.
      item.next = END;
      if (!entry.occupied) {
        entry = {leaf, groups_.size(), item.op.key, true};
        groups_.push_back({leaf, pos, pos, 1});
      } else {
        auto& g = groups_[entry.group_index];
        batch_[g.last].next = pos;
        g.last = pos;
        ++g.count;
      }
      groups_[entry.group_index].materialize = (reserved == available);
    }
    return pos;
  }

  bool run_wave() {
    // Small waves cannot amortize a worker-pool handoff. Keep this threshold
    // explicit and report how many updates actually enter parallel waves.
    if (groups_.size() == 1 || opts_.workers == 1 || groups_.size() < opts_.parallel_min_groups) {
      phase_timer timer(inline_leaf_ns_);
      for (size_t i=0; i<groups_.size(); ++i) execute_group(i);
      return false;
    }
    phase_timer timer(parallel_wave_ns_);
    {
      std::lock_guard lock(pool_mutex_);
      next_group_.store(0, std::memory_order_relaxed);
      remaining_ = workers_.size();
      ++generation_;
    }
    work_cv_.notify_all();
    std::unique_lock lock(pool_mutex_);
    done_cv_.wait(lock, [&] { return remaining_ == 0; });
    if (worker_failure_) std::rethrow_exception(worker_failure_);
    return true;
  }

  void publish(uint64_t ticket) {
    {
    phase_timer timer(publish_ns_);
    gc_.publish(root_, ticket);
    // Lock couples this state change with commit_cv_'s predicate to avoid a
    // missed wakeup between a waiter checking committed_ and going to sleep.
    {
      std::lock_guard lock(input_mutex_);
      committed_.store(ticket, std::memory_order_release);
    }
    commit_cv_.notify_all();
    }
    // No workers use retired nodes here; externally triggered collections use
    // this same committed frontier and the reclaimer's reader pins.
    phase_timer timer(gc_ns_);
    gc_.try_reclaim(ticket);
  }

  void cow_update(const operation& op) {
    {
    phase_timer timer(serial_cow_ns_);
    typename T::context ctx{};
    ctx.sno = static_cast<uint32_t>(op.ticket);
    ctx.key = op.key;
    ctx.val = op.value;
    ctx.t_past = root_;
    ctx.fat_slots = opts_.slots;
    ctx.gc_enabled = opts_.gc;
    ctx.op = T::concurrent_fat_cow_begin(&ctx);
    while (ctx.op != T::operand::DONE) ctx.op = T::handlers[ctx.op](&ctx);
    if (ctx.new_checkpoint) {
      // The update path is complete and workers are quiescent. Rebuild before
      // publishing, retiring both the old path and the intermediate shell.
      ctx.t_past = ctx.root;
      T::checkpoint_handler(&ctx);
      ctx.checkpoint_rebuilt = true;
    }
    root_ = ctx.root;
    gc_.retire(op.ticket, &ctx);
    cows_.fetch_add(1, std::memory_order_relaxed);
    }
    publish(op.ticket);
  }

  void coordinator_main() {
    try {
      uint64_t next = 1;
      for (;;) {
        {
          phase_timer timer(queue_ns_);
          std::unique_lock lock(input_mutex_);
          input_cv_.wait(lock, [&] { return stopping_ || submitted_ >= next; });
          if (submitted_ < next) break;
          const size_t count = std::min<uint64_t>(opts_.batch_size, submitted_-next+1);
          batch_.clear();
          for (size_t i=0; i<count; ++i) batch_.push_back({queue_[(next+i)%queue_.size()], END});
        }
        for (size_t first=0; first<batch_.size();) {
          const size_t end = (opts_.slots || opts_.parallel_materialization) ? plan_wave(first) : first;
          if (end == first) {
            cow_update(batch_[first].op);
            ++first;
          } else {
            const bool parallel = run_wave();
            size_t materialized = 0;
            {
            phase_timer timer(merge_retire_ns_);
            std::vector<typename T::concurrent_fat_replacement*> replacements;
            for(auto& g : groups_) if(g.materialize) {
              replacements.push_back(&g.replacement); ++materialized;
            }
            if(materialized) {
              typename T::concurrent_fat_merge merged;
              merged.build(root_, replacements, static_cast<uint32_t>(batch_[end-1].op.ticket));
              gc_.retire(batch_[end-1].op.ticket, &merged);
              merged.release(replacements);
              root_ = merged.root;
              materialization_waves_.fetch_add(1, std::memory_order_relaxed);
              cows_.fetch_add(materialized, std::memory_order_relaxed);
              if(parallel) parallel_materializations_.fetch_add(materialized, std::memory_order_relaxed);
            }
            }
            if (parallel) {
              parallel_waves_.fetch_add(1, std::memory_order_relaxed);
              parallel_updates_.fetch_add(end-first, std::memory_order_relaxed);
            }
            appended_.fetch_add(end-first-materialized, std::memory_order_relaxed);
            waves_.fetch_add(1, std::memory_order_relaxed);
            max_groups_.store(std::max<uint64_t>(max_groups_.load(std::memory_order_relaxed), groups_.size()), std::memory_order_relaxed);
            publish(batch_[end-1].op.ticket);
            first = end;
          }
        }
        next = batch_.back().op.ticket+1;
      }
    } catch (...) {
      {
        std::lock_guard lock(input_mutex_);
        failure_ = std::current_exception();
        stopping_ = true;
      }
      commit_cv_.notify_all();
    }
  }

  void stop_pool() {
    { std::lock_guard lock(pool_mutex_); pool_stopping_ = true; }
    work_cv_.notify_all();
    for (auto& thread : workers_) if (thread.joinable()) thread.join();
  }

public:
  explicit concow_fat(nodeptr root, options opts = {}) :
    opts_(validate(opts)), gc_(root, opts_.readers, opts_.gc, true), root_(root),
    queue_(opts_.queue_capacity), table_(std::bit_ceil(2*opts_.batch_size)) {
    if (!root) {
      if constexpr (!requires { T::allow_empty_root; })
        throw std::invalid_argument("this tree requires a non-null initial root");
      else if(!T::allow_empty_root)throw std::invalid_argument("empty root is unsupported");
    }
    batch_.reserve(opts_.batch_size);
    groups_.reserve(opts_.batch_size);
    workers_.reserve(opts_.workers);
    try {
      for (unsigned i=0; i<opts_.workers; ++i) workers_.emplace_back([this] { worker_main(); });
      coordinator_ = std::thread([this] { coordinator_main(); });
    } catch (...) { stop_pool(); throw; }
  }
  concow_fat(const concow_fat&) = delete;
  concow_fat& operator=(const concow_fat&) = delete;

  ~concow_fat() {
    { std::lock_guard lock(input_mutex_); stopping_ = true; }
    input_cv_.notify_one();
    coordinator_.join(); // drain every accepted operation before stopping workers
    stop_pool();
    gc_.try_reclaim(committed_.load(std::memory_order_acquire));
  }

  uint64_t update(uint64_t key, uint64_t value) {
    std::unique_lock lock(input_mutex_);
    commit_cv_.wait(lock, [&] { return failure_ || stopping_ || submitted_-committed_.load(std::memory_order_acquire) < queue_.size(); });
    if (failure_) std::rethrow_exception(failure_);
    if (stopping_) throw std::logic_error("controller is stopping");
    if (submitted_ == UINT32_MAX) throw std::overflow_error("fat-node version exhausted");
    const uint64_t ticket = ++submitted_;
    queue_[ticket%queue_.size()] = {ticket, key, value};
    lock.unlock();
    input_cv_.notify_one();
    return ticket; // asynchronous submission, as in legacy ConCow/SeqCow
  }

  void wait_for_processing(uint64_t ticket) {
    std::unique_lock lock(input_mutex_);
    commit_cv_.wait(lock, [&] { return failure_ || committed_.load(std::memory_order_acquire) >= ticket; });
    if (failure_) std::rethrow_exception(failure_);
  }

  template<class F> auto query(uint64_t cid, F&& f) {
    if (cid >= opts_.readers) throw std::out_of_range("reader id");
    auto guard = gc_.pin(cid);
    return f(guard.root(), guard.version()); // version-aware callbacks required
  }
  template<class F> auto query(F&& f) { return query(0, std::forward<F>(f)); }
  void collect_garbage() { gc_.try_reclaim(committed_.load(std::memory_order_acquire)); }
  auto gc_statistics() const { return gc_.get_statistics(); }
  statistics execution_statistics() const {
    return {appended_.load(), cows_.load(), waves_.load(), max_groups_.load(), parallel_waves_.load(), parallel_updates_.load(), materialization_waves_.load(), parallel_materializations_.load()};
  }
  phase_statistics phase_timings() const {
    return {queue_ns_.load(),plan_ns_.load(),inline_leaf_ns_.load(),parallel_wave_ns_.load(),
      merge_retire_ns_.load(),publish_ns_.load(),gc_ns_.load(),serial_cow_ns_.load(),
      worker_loop_ns_.load(),worker_wakeups_.load()};
  }
};
} // namespace conctrl
