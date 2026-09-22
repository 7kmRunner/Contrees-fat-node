#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace conctrl {

/*
 * Reader-version based reclamation for path-copying trees.
 *
 * A reader first advertises the version it intends to use and then validates
 * an atomic snapshot sequence.  The seq_cst ordering gives the usual hazard
 * pointer guarantee: either a concurrent publisher observes the advertised
 * reader, or the reader observes the publication and retries before
 * dereferencing the old root.
 */
// SingleOwner requires retire/try_reclaim to run on one designated thread
// (or after that thread has joined). Readers only touch atomic snapshot slots.
// Other controllers keep the shared, mutex-protected default.
template <typename T, bool SingleOwner = false>
class epoch_reclaimer {
public:
  using nodeptr = typename T::nodeptr;
  using cnodeptr = typename T::cnodeptr;

  static inline constexpr uint64_t INACTIVE =
    std::numeric_limits<uint64_t>::max();

  struct statistics {
    uint64_t retired_nodes;
    uint64_t reclaimed_nodes;
    uint64_t pending_nodes;
    uint64_t retired_bytes;
    uint64_t reclaimed_bytes;
    uint64_t pending_bytes;
  };

  struct snapshot {
    cnodeptr root;
    uint64_t version;
  };

private:
  struct alignas(128) reader_slot {
    std::atomic_uint64_t version;

    reader_slot() : version(INACTIVE) {}
  };

  struct retire_batch {
    /* First published version from whose root these nodes are unreachable. */
    uint64_t version;
    uint64_t bytes;
    uint64_t node_count;
    std::vector<nodeptr> nodes;
    nodeptr checkpoint_root;
  };

  const bool enabled_;
  /* Fat-node readers need an atomic root/version pair even without freeing. */
  const bool consistent_snapshot_;
  const uint64_t n_readers_;
  std::unique_ptr<reader_slot[]> readers_;

  /* sequence is even outside publication and odd while root/version change */
  alignas(128) std::atomic_uint64_t published_sequence_;
  std::atomic<nodeptr> published_root_;
  std::atomic_uint64_t published_version_;

  struct single_owner_queue {};
  [[no_unique_address]] std::conditional_t<SingleOwner, single_owner_queue,
                                         std::mutex> retired_mutex_;
  std::deque<retire_batch> retired_;

  std::atomic_uint64_t retired_nodes_;
  std::atomic_uint64_t reclaimed_nodes_;
  std::atomic_uint64_t retired_bytes_;
  std::atomic_uint64_t reclaimed_bytes_;

  template <typename F>
  void access_retired_(F&& f) {
    if constexpr (SingleOwner) {
      std::forward<F>(f)();
    } else {
      std::lock_guard lock(retired_mutex_);
      std::forward<F>(f)();
    }
  }

  static uint64_t node_size_(nodeptr node) {
    if constexpr (requires { T::node_size(node); }) {
      return static_cast<uint64_t>(T::node_size(node));
    } else if constexpr (requires { T::node_size(); }) {
      return static_cast<uint64_t>(T::node_size());
    } else if constexpr (requires { T::node_size; }) {
      return static_cast<uint64_t>(T::node_size);
    } else {
      return 0;
    }
  }

  uint64_t oldest_active_reader_() const {
    uint64_t oldest = published_version_.load(std::memory_order_seq_cst);
    for (uint64_t i = 0; i < n_readers_; ++i) {
      const uint64_t version =
        readers_[i].version.load(std::memory_order_seq_cst);
      if (version != INACTIVE) { oldest = std::min(oldest, version); }
    }
    return oldest;
  }

  void unpin_(uint64_t cid) {
    if (!enabled_) { return; }
    assert(cid < n_readers_);
    readers_[cid].version.store(INACTIVE, std::memory_order_seq_cst);
  }

public:
  class read_guard {
  private:
    epoch_reclaimer* owner_;
    uint64_t cid_;
    snapshot snapshot_;

    friend class epoch_reclaimer;
    read_guard(epoch_reclaimer* owner, uint64_t cid, snapshot value) :
      owner_(owner), cid_(cid), snapshot_(value) {}

  public:
    read_guard(const read_guard&) = delete;
    read_guard& operator=(const read_guard&) = delete;

    read_guard(read_guard&& other) noexcept :
      owner_(std::exchange(other.owner_, nullptr)),
      cid_(other.cid_), snapshot_(other.snapshot_) {}

    ~read_guard() {
      if (owner_ != nullptr) { owner_->unpin_(cid_); }
    }

    cnodeptr root() const { return snapshot_.root; }
    uint64_t version() const { return snapshot_.version; }
  };

  epoch_reclaimer(nodeptr initial_root, uint64_t n_readers, bool enabled,
                  bool consistent_snapshot = false) :
    enabled_(enabled),
    consistent_snapshot_(enabled || consistent_snapshot),
    n_readers_(enabled ? std::max<uint64_t>(n_readers, 1) : 0),
    readers_(enabled ? std::make_unique<reader_slot[]>(n_readers_) : nullptr),
    published_sequence_(0),
    published_root_(initial_root),
    published_version_(0),
    retired_nodes_(0), reclaimed_nodes_(0),
    retired_bytes_(0), reclaimed_bytes_(0)
  {}

  bool enabled() const { return enabled_; }

  /* Single-publisher operation, called by the worker/monitor commit path. */
  void publish(nodeptr root, uint64_t version) {
    if (!consistent_snapshot_) {
      published_root_.store(root, std::memory_order_release);
      published_version_.store(version, std::memory_order_release);
      return;
    }

    published_sequence_.fetch_add(1, std::memory_order_seq_cst);
    published_root_.store(root, std::memory_order_seq_cst);
    published_version_.store(version, std::memory_order_seq_cst);
    published_sequence_.fetch_add(1, std::memory_order_seq_cst);
  }

  read_guard pin(uint64_t cid) {
    if (!consistent_snapshot_) {
      return read_guard(this, 0, {
        published_root_.load(std::memory_order_acquire),
        published_version_.load(std::memory_order_acquire) });
    }

    if (enabled_) { assert(cid < n_readers_); }
    while (true) {
      const uint64_t sequence =
        published_sequence_.load(std::memory_order_seq_cst);
      if (sequence & 1) { continue; }

      const snapshot candidate {
        published_root_.load(std::memory_order_seq_cst),
        published_version_.load(std::memory_order_seq_cst) };

      if (enabled_) {
        readers_[cid].version.store(candidate.version,
                                    std::memory_order_seq_cst);
      }

      if (sequence ==
          published_sequence_.load(std::memory_order_seq_cst)) {
        return read_guard(this, enabled_ ? cid : 0, candidate);
      }

      if (enabled_) {
        readers_[cid].version.store(INACTIVE, std::memory_order_seq_cst);
      }
    }
  }

  template <typename Context>
  void retire(uint64_t version, Context* ctx) {
    if (!enabled_) {
      /* Normally empty because gc_enabled is false in the context. */
      ctx->clear_retired();
      return;
    }

    retire_batch batch { version, 0, 0, {}, nullptr };
    batch.nodes.reserve(ctx->retired_count);
    ctx->drain_retired([&](nodeptr node) {
      batch.nodes.push_back(node);
      batch.bytes += node_size_(node);
      ++batch.node_count;
    });

    if (ctx->checkpoint_rebuilt) {
      if constexpr (requires(nodeptr root) {
        T::checkpoint_retired_stats(root);
        T::free_checkpoint(root);
      }) {
        batch.checkpoint_root = ctx->t_past;
        const auto [nodes, bytes] =
          T::checkpoint_retired_stats(batch.checkpoint_root);
        batch.node_count += nodes;
        batch.bytes += bytes;
      }
    }

    if (batch.node_count == 0) { return; }

    retired_nodes_.fetch_add(batch.node_count, std::memory_order_relaxed);
    retired_bytes_.fetch_add(batch.bytes, std::memory_order_relaxed);
    access_retired_([&] { retired_.push_back(std::move(batch)); });
  }

  /*
   * writer_safe_version is the largest retirement version for which no
   * pipeline/worker can still hold an update-side pointer.
   */
  void try_reclaim(uint64_t writer_safe_version) {
    if (!enabled_) { return; }

    const uint64_t reader_safe_version = oldest_active_reader_();
    const uint64_t safe_version =
      std::min(reader_safe_version, writer_safe_version);

    uint64_t reclaimed_nodes = 0;
    uint64_t reclaimed_bytes = 0;
    access_retired_([&] {
      /*
       * Equality is safe: a reader pinned at the retirement version starts at
       * the first root which no longer reaches the retired physical nodes.
       */
      while (!retired_.empty() && retired_.front().version <= safe_version) {
        auto& batch = retired_.front();
        for (nodeptr node : batch.nodes) { T::free_node(node); }
        if (batch.checkpoint_root != nullptr) {
          if constexpr (requires(nodeptr root) { T::free_checkpoint(root); }) {
            T::free_checkpoint(batch.checkpoint_root);
          }
        }
        reclaimed_nodes += batch.node_count;
        reclaimed_bytes += batch.bytes;
        retired_.pop_front();
      }
    });

    reclaimed_nodes_.fetch_add(reclaimed_nodes, std::memory_order_relaxed);
    reclaimed_bytes_.fetch_add(reclaimed_bytes, std::memory_order_relaxed);
  }

  statistics get_statistics() const {
    const uint64_t retired_nodes =
      retired_nodes_.load(std::memory_order_relaxed);
    const uint64_t reclaimed_nodes =
      reclaimed_nodes_.load(std::memory_order_relaxed);
    const uint64_t retired_bytes =
      retired_bytes_.load(std::memory_order_relaxed);
    const uint64_t reclaimed_bytes =
      reclaimed_bytes_.load(std::memory_order_relaxed);
    return {
      retired_nodes,
      reclaimed_nodes,
      retired_nodes - reclaimed_nodes,
      retired_bytes,
      reclaimed_bytes,
      retired_bytes - reclaimed_bytes };
  }
};

} // namespace conctrl
