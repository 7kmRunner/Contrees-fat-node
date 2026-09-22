#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "lib/conctrl/concow.hpp"
#include "lib/conctrl/seqcow.hpp"
#include "src/adapters/betree.hpp"
#include "utils/memory_stats.hpp"

namespace {

struct options {
  uint64_t records = 65536;
  uint64_t updates = 200000;
  uint64_t pipes = 1;
  uint64_t workers = 2;
  bool gc_enabled = false;
  bool reader_safety_test = false;
  bool checkpoint_reclaim_test = false;
  std::string_view scheduler = "seqcow";
};

[[noreturn]] void usage(const char* program) {
  std::fprintf(stderr,
    "usage: %s [--gc] [--scheduler seqcow|concow] [--records N] "
    "[--updates N] [--pipes N] [--workers N] [--reader-safety-test] "
    "[--checkpoint-reclaim-test]\n",
    program);
  std::exit(2);
}

uint64_t parse_u64(const char* program, std::string_view text) {
  uint64_t value = 0;
  const auto [end, error] =
    std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
    usage(program);
  }
  return value;
}

options parse_options(int argc, char** argv) {
  options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--gc") {
      opts.gc_enabled = true;
    } else if (arg == "--reader-safety-test") {
      opts.reader_safety_test = true;
      opts.gc_enabled = true;
    } else if (arg == "--checkpoint-reclaim-test") {
      opts.checkpoint_reclaim_test = true;
      opts.gc_enabled = true;
    } else if (arg == "--scheduler" && i + 1 < argc) {
      opts.scheduler = argv[++i];
    } else if (arg == "--records" && i + 1 < argc) {
      opts.records = parse_u64(argv[0], argv[++i]);
    } else if (arg == "--updates" && i + 1 < argc) {
      opts.updates = parse_u64(argv[0], argv[++i]);
    } else if (arg == "--pipes" && i + 1 < argc) {
      opts.pipes = parse_u64(argv[0], argv[++i]);
    } else if (arg == "--workers" && i + 1 < argc) {
      opts.workers = parse_u64(argv[0], argv[++i]);
    } else {
      usage(argv[0]);
    }
  }
  if (opts.scheduler != "seqcow" && opts.scheduler != "concow") {
    usage(argv[0]);
  }
  return opts;
}

bool run_checkpoint_case(uint64_t records, uint64_t pipes) {
  std::vector<kv> elems(records);
  for (uint64_t i = 0; i < records; ++i) { elems[i] = {i, i}; }

  config cfg;
  cfg.num_pipes = pipes;
  auto old_root = betree::interface::build(cfg, records, elems.data());

  betree::context ctx{};
  ctx.gc_enabled = 1;
  ctx.checkpoint_rebuilt = 1;
  ctx.t_past = old_root;
  betree::interface::checkpoint_handler(&ctx);

  const bool entire =
    betree::leaf_over_boundary(old_root, old_root->verge);
  const auto expected =
    betree::interface::checkpoint_retired_stats(old_root);
  conctrl::epoch_reclaimer<betree::interface> gc(old_root, 1, true);
  gc.retire(1, &ctx);
  gc.publish(ctx.root, 1);
  gc.try_reclaim(1);
  const auto stats = gc.get_statistics();

  bool values_ok = true;
  for (uint64_t i = 0; i < records; i += 97) {
    const auto value = betree::interface::find(ctx.root, i);
    values_ok &= value.has_value() && *value == i;
  }
  const bool stats_ok = expected.first > 0 &&
    stats.retired_nodes == expected.first &&
    stats.retired_bytes == expected.second &&
    stats.reclaimed_nodes == expected.first && stats.pending_nodes == 0;
  std::printf(
    "checkpoint_mode=%s pipes=%llu retired_nodes=%llu values=%s stats=%s\n",
    entire ? "entire" : "upper",
    static_cast<unsigned long long>(pipes),
    static_cast<unsigned long long>(expected.first),
    values_ok ? "pass" : "fail", stats_ok ? "pass" : "fail");
  return values_ok && stats_ok;
}

int run_checkpoint_reclaim_test(const options& opts) {
  const bool upper_ok = run_checkpoint_case(opts.records, 4);
  const bool entire_ok = run_checkpoint_case(opts.records, 10);
  return upper_ok && entire_ok ? 0 : 1;
}

template <typename Scheduler>
uint64_t submit_updates(Scheduler& scheduler, uint64_t updates,
                        uint64_t value_base) {
  uint64_t last_tid = 0;
  for (uint64_t i = 0; i < updates; ++i) {
    last_tid = scheduler.update(0, value_base + i + 1);
  }
  scheduler.wait_for_processing(last_tid);
  return value_base + updates;
}

template <typename Scheduler>
bool current_value_is(Scheduler& scheduler, uint64_t expected) {
  const auto actual = scheduler.query(0, [](auto root) {
    return betree::interface::find(root, 0);
  });
  return actual.has_value() && *actual == expected;
}

template <typename Scheduler>
int run_reader_safety_test(Scheduler& scheduler, const options& opts) {
  std::atomic_bool pinned = false;
  std::atomic_bool release_reader = false;
  uint64_t reader_value = UINT64_MAX;

  std::thread reader([&] {
    const auto value = scheduler.query(0, [&](auto root) {
      pinned.store(true, std::memory_order_release);
      while (!release_reader.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      return betree::interface::find(root, 0);
    });
    if (value.has_value()) { reader_value = *value; }
  });

  while (!pinned.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  const uint64_t expected = submit_updates(scheduler, opts.updates, opts.records);
  const auto blocked = scheduler.gc_statistics();

  release_reader.store(true, std::memory_order_release);
  reader.join();
  scheduler.collect_garbage();
  const auto released = scheduler.gc_statistics();

  const bool passed = reader_value == 0 && blocked.pending_nodes > 0 &&
    released.pending_nodes == 0 && current_value_is(scheduler, expected);
  std::printf(
    "reader_safety=%s old_reader_value=%llu pending_while_pinned=%llu "
    "pending_after_release=%llu\n",
    passed ? "pass" : "fail",
    static_cast<unsigned long long>(reader_value),
    static_cast<unsigned long long>(blocked.pending_nodes),
    static_cast<unsigned long long>(released.pending_nodes));
  return passed ? 0 : 1;
}

template <typename Scheduler>
int run_workload(Scheduler& scheduler, const options& opts) {
  const auto started = std::chrono::steady_clock::now();
  const uint64_t expected = submit_updates(scheduler, opts.updates, opts.records);
  scheduler.collect_garbage();
  const auto stopped = std::chrono::steady_clock::now();
  const auto stats = scheduler.gc_statistics();
  const double elapsed = std::chrono::duration<double>(stopped - started).count();
  const bool value_ok = current_value_is(scheduler, expected);
  const bool gc_ok = !opts.gc_enabled ||
    (stats.retired_nodes > 0 && stats.pending_nodes == 0 &&
     stats.retired_nodes == stats.reclaimed_nodes);

  std::printf(
    "scheduler=%.*s gc=%s records=%llu updates=%llu elapsed_seconds=%.6f "
    "value_check=%s\n",
    static_cast<int>(opts.scheduler.size()), opts.scheduler.data(),
    opts.gc_enabled ? "on" : "off",
    static_cast<unsigned long long>(opts.records),
    static_cast<unsigned long long>(opts.updates), elapsed,
    value_ok ? "pass" : "fail");
  std::printf(
    "retired_nodes=%llu reclaimed_nodes=%llu pending_nodes=%llu "
    "retired_bytes=%llu reclaimed_bytes=%llu pending_bytes=%llu\n",
    static_cast<unsigned long long>(stats.retired_nodes),
    static_cast<unsigned long long>(stats.reclaimed_nodes),
    static_cast<unsigned long long>(stats.pending_nodes),
    static_cast<unsigned long long>(stats.retired_bytes),
    static_cast<unsigned long long>(stats.reclaimed_bytes),
    static_cast<unsigned long long>(stats.pending_bytes));
  std::printf("current_rss_bytes=%llu peak_rss_bytes=%llu\n",
    static_cast<unsigned long long>(
      contrees::memory_stats::current_rss_bytes()),
    static_cast<unsigned long long>(
      contrees::memory_stats::peak_rss_bytes()));
  return value_ok && gc_ok ? 0 : 1;
}

template <typename Scheduler>
int run(Scheduler& scheduler, const options& opts) {
  if (opts.reader_safety_test) {
    return run_reader_safety_test(scheduler, opts);
  }
  return run_workload(scheduler, opts);
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  const options opts = parse_options(argc, argv);
  if (opts.checkpoint_reclaim_test) {
    return run_checkpoint_reclaim_test(opts);
  }
  std::vector<kv> elems(opts.records);
  for (uint64_t i = 0; i < opts.records; ++i) {
    elems[i] = {i, i};
  }

  config cfg;
  cfg.num_pipes = opts.pipes;
  auto root = betree::interface::build(cfg, elems.size(), elems.data());

  if (opts.scheduler == "seqcow") {
    auto scheduler = std::make_unique<conctrl::seqcow<betree::interface>>(
      root, 1, opts.gc_enabled);
    return run(*scheduler, opts);
  }

  auto scheduler = std::make_unique<conctrl::concow<betree::interface>>(
    opts.pipes, opts.workers, root, 1, opts.gc_enabled);
  return run(*scheduler, opts);
}
