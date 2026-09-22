#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "lib/conctrl/seqcow.hpp"
#include "src/adapters/betree.hpp"
#include "utils/memory_stats.hpp"

namespace {

/* Keep this byte-for-byte compatible with src/main.cpp and ycsbc/db/export.hpp. */
struct tx_context {
  uint8_t type;
  uint64_t arg0;
  uint64_t arg1;
};

static_assert(sizeof(size_t) == 8,
              "the existing workload format requires a 64-bit size_t");
static_assert(sizeof(tx_context) == 24);
static_assert(offsetof(tx_context, arg0) == 8);
static_assert(offsetof(tx_context, arg1) == 16);

struct options {
  std::filesystem::path dataset;
  uint8_t fat_slots = 0;
  uint64_t clients = 1;
  uint64_t num_pipes = 1;
  bool gc_enabled = false;
  bool slots_given = false;
};

struct workload {
  std::vector<uint64_t> records;
  std::vector<tx_context> transactions;
};

struct operation_counts {
  uint64_t reads = 0;
  uint64_t scans = 0;
  uint64_t updates = 0;
  uint64_t ignored = 0;
};

struct live_tree_stats {
  uint64_t nodes = 0;
  uint64_t internal_nodes = 0;
  uint64_t leaf_nodes = 0;
  uint64_t sidecar_leaves = 0;
  uint64_t delta_records = 0;
  uint64_t requested_bytes = 0;
  uint64_t sidecar_requested_bytes = 0;
};

[[noreturn]] void usage(const char* program, const std::string& error = {}) {
  if (!error.empty()) { std::cerr << "error: " << error << "\n\n"; }
  std::cerr << "Usage: " << program
            << " <dataset> --fat-slots <0|2|4|8> [--gc] [-c <clients>]"
            << " [-p <num_pipes>]\n";
  std::exit(error.empty() ? EXIT_SUCCESS : EXIT_FAILURE);
}

uint64_t parse_uint(std::string_view text, const char* name) {
  uint64_t value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [next, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || next != end) {
    throw std::runtime_error(std::string("invalid ") + name + ": " +
                             std::string(text));
  }
  return value;
}

options parse_options(int argc, char** argv) {
  if (argc < 2) { usage(argv[0], "missing dataset"); }
  if (std::string_view(argv[1]) == "-h" ||
      std::string_view(argv[1]) == "--help") {
    usage(argv[0]);
  }

  options result;
  result.dataset = argv[1];

  for (int i = 2; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--fat-slots") {
      if (++i >= argc) { usage(argv[0], "--fat-slots needs a value"); }
      const uint64_t value = parse_uint(argv[i], "fat slot count");
      if (value != 0 && value != 2 && value != 4 && value != 8) {
        usage(argv[0], "--fat-slots must be 0, 2, 4, or 8");
      }
      result.fat_slots = static_cast<uint8_t>(value);
      result.slots_given = true;
    } else if (argument.starts_with("--fat-slots=")) {
      const uint64_t value = parse_uint(
          argument.substr(std::string_view("--fat-slots=").size()),
          "fat slot count");
      if (value != 0 && value != 2 && value != 4 && value != 8) {
        usage(argv[0], "--fat-slots must be 0, 2, 4, or 8");
      }
      result.fat_slots = static_cast<uint8_t>(value);
      result.slots_given = true;
    } else if (argument == "-c" || argument == "--clients") {
      if (++i >= argc) { usage(argv[0], "client option needs a value"); }
      result.clients = parse_uint(argv[i], "client count");
      if (result.clients == 0) {
        usage(argv[0], "client count must be positive");
      }
    } else if (argument == "-p" || argument == "--pipes") {
      if (++i >= argc) { usage(argv[0], "pipe option needs a value"); }
      result.num_pipes = parse_uint(argv[i], "pipe count");
      if (result.num_pipes == 0 || result.num_pipes > 256) {
        usage(argv[0], "pipe count must be between 1 and 256");
      }
    } else if (argument == "-g" || argument == "--gc") {
      result.gc_enabled = true;
    } else if (argument == "-h" || argument == "--help") {
      usage(argv[0]);
    } else {
      usage(argv[0], "unknown option: " + std::string(argument));
    }
  }

  if (!result.slots_given) {
    usage(argv[0], "--fat-slots is required");
  }
  if (result.clients > std::numeric_limits<uint32_t>::max()) {
    usage(argv[0], "client count is too large");
  }
  return result;
}

uintmax_t checked_workload_size(size_t records, size_t transactions) {
  constexpr uintmax_t HEADER_BYTES = 2 * sizeof(size_t);
  constexpr uintmax_t RECORD_BYTES = sizeof(uint64_t);
  constexpr uintmax_t TX_BYTES = sizeof(tx_context);
  constexpr uintmax_t LIMIT = std::numeric_limits<uintmax_t>::max();

  if (records > (LIMIT - HEADER_BYTES) / RECORD_BYTES) {
    throw std::runtime_error("record count overflows workload size");
  }
  const uintmax_t after_records =
      HEADER_BYTES + static_cast<uintmax_t>(records) * RECORD_BYTES;
  if (transactions > (LIMIT - after_records) / TX_BYTES) {
    throw std::runtime_error("transaction count overflows workload size");
  }
  return after_records + static_cast<uintmax_t>(transactions) * TX_BYTES;
}

template <typename T>
void read_exact(std::ifstream& input, T* destination, size_t count,
                const char* description) {
  if (count == 0) { return; }
  const uintmax_t byte_count = static_cast<uintmax_t>(count) * sizeof(T);
  if (byte_count >
      static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    throw std::runtime_error(std::string(description) + " is too large");
  }
  input.read(reinterpret_cast<char*>(destination),
             static_cast<std::streamsize>(byte_count));
  if (!input) {
    throw std::runtime_error(std::string("failed to read ") + description);
  }
}

workload load_workload(const std::filesystem::path& path) {
  std::error_code file_error;
  const uintmax_t actual_size = std::filesystem::file_size(path, file_error);
  if (file_error) {
    throw std::runtime_error("cannot stat dataset '" + path.string() +
                             "': " + file_error.message());
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open dataset '" + path.string() + "'");
  }

  size_t record_count = 0;
  size_t transaction_count = 0;
  read_exact(input, &record_count, 1, "record count");
  read_exact(input, &transaction_count, 1, "transaction count");

  const uintmax_t expected_size =
      checked_workload_size(record_count, transaction_count);
  if (actual_size != expected_size) {
    throw std::runtime_error(
        "dataset size does not match the src/main.cpp layout: expected " +
        std::to_string(expected_size) + " bytes, found " +
        std::to_string(actual_size));
  }

  workload result;
  result.records.resize(record_count);
  result.transactions.resize(transaction_count);
  read_exact(input, result.records.data(), result.records.size(), "records");
  read_exact(input, result.transactions.data(), result.transactions.size(),
             "transactions");
  return result;
}

operation_counts count_operations(const std::vector<tx_context>& txs) {
  operation_counts result;
  for (const tx_context& tx : txs) {
    if (tx.type == 0) {
      if (tx.arg1 == 0) {
        ++result.reads;
      } else {
        ++result.scans;
      }
    } else if (tx.type == 1) {
      ++result.updates;
    } else {
      /* Match src/main.cpp: operation types other than query/update are no-op. */
      ++result.ignored;
    }
  }
  return result;
}

uint64_t mix(uint64_t value) {
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

using scheduler_type = conctrl::seqcow<betree::interface>;

uint64_t execute_transactions(uint64_t clients,
                              const std::vector<tx_context>& txs,
                              scheduler_type& scheduler) {
  static constexpr uint64_t BATCH_SIZE = 64;
  alignas(128) std::atomic_uint64_t cursor(0);
  alignas(128) std::atomic_uint64_t submitted(0);
  alignas(128) std::atomic_uint64_t checksum(0);
  std::vector<std::thread> threads;
  threads.reserve(clients);

  for (uint64_t cid = 0; cid < clients; ++cid) {
    threads.emplace_back([&, cid] {
      uint64_t local_submitted = 0;
      uint64_t local_checksum = mix(cid + 1);

      while (true) {
        const uint64_t begin =
            cursor.fetch_add(BATCH_SIZE, std::memory_order_acq_rel);
        if (begin >= txs.size()) { break; }
        const uint64_t end =
            std::min<uint64_t>(begin + BATCH_SIZE, txs.size());

        for (uint64_t i = begin; i < end; ++i) {
          const tx_context& tx = txs[i];
          if (tx.type == 0) {
            if (tx.arg1 == 0) {
              const uint64_t observed = scheduler.query(
                  cid, [key = tx.arg0](auto root, uint64_t version) {
                    const auto value =
                        betree::interface::find(root, key, version);
                    return value.value_or(UINT64_MAX);
                  });
              local_checksum ^= mix(tx.arg0 ^ observed ^ i);
            } else {
              const uint64_t observed = scheduler.query(
                  cid, [key = tx.arg0, length = tx.arg1](
                           auto root, uint64_t version) {
                    const auto values =
                        betree::interface::scan(root, key, length, version);
                    uint64_t scan_checksum = mix(values.size());
                    for (const auto& [found_key, found_value] : values) {
                      scan_checksum ^= mix(found_key) ^ mix(found_value);
                    }
                    return scan_checksum;
                  });
              local_checksum ^= mix(observed ^ i);
            }
          } else if (tx.type == 1) {
            scheduler.update(tx.arg0, tx.arg1);
            ++local_submitted;
          }
        }
      }

      submitted.fetch_add(local_submitted, std::memory_order_relaxed);
      checksum.fetch_xor(local_checksum, std::memory_order_relaxed);
    });
  }

  for (std::thread& client : threads) { client.join(); }
  scheduler.wait_for_processing(submitted.load(std::memory_order_acquire));
  return checksum.load(std::memory_order_relaxed);
}

double seconds_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now() - start)
      .count();
}

void collect_live_tree_stats(betree::cnodeptr node, live_tree_stats& stats) {
  if (node == nullptr) { return; }
  ++stats.nodes;
  stats.requested_bytes += betree::node_size(node);

  if (node->type == betree::LEAF) {
    ++stats.leaf_nodes;
    if (const betree::delta_block* block = betree::leaf_delta(node);
        block != nullptr) {
      ++stats.sidecar_leaves;
      stats.delta_records += betree::leaf_delta_count(node);
      stats.sidecar_requested_bytes +=
          betree::delta_block_size(block->capacity);
    }
    return;
  }

  ++stats.internal_nodes;
  for (uint8_t i = 0; i <= node->size; ++i) {
    collect_live_tree_stats(node->chs[i], stats);
  }
}

int run(const options& opts) {
  const workload data = load_workload(opts.dataset);
  const operation_counts counts = count_operations(data.transactions);

  std::vector<kv> initial_values;
  initial_values.reserve(data.records.size());
  for (const uint64_t key : data.records) {
    initial_values.emplace_back(key, key);
  }

  config build_config;
  build_config.fat_slots = opts.fat_slots;
  build_config.num_pipes = opts.num_pipes;

  const auto init_start = std::chrono::steady_clock::now();
  auto root = betree::interface::build(
      build_config, initial_values.size(), initial_values.data());
  auto scheduler = std::make_unique<scheduler_type>(
      root, opts.clients, opts.gc_enabled, opts.fat_slots);
  const double init_seconds = seconds_since(init_start);

  const auto process_start = std::chrono::steady_clock::now();
  const uint64_t checksum = execute_transactions(
      opts.clients, data.transactions, *scheduler);
  const double process_seconds = seconds_since(process_start);

  const auto collect_start = std::chrono::steady_clock::now();
  scheduler->collect_garbage();
  const double collect_seconds = seconds_since(collect_start);

  const auto gc = scheduler->gc_statistics();
  const live_tree_stats live = scheduler->query(
      0, [](auto published_root, uint64_t) {
        live_tree_stats result;
        collect_live_tree_stats(published_root, result);
        return result;
      });
  const auto memory = contrees::memory_stats::sample_now();

  std::cout << std::setprecision(9) << std::fixed;
  std::cout << "fat_slots=" << static_cast<unsigned>(opts.fat_slots) << '\n';
  std::cout << "gc_enabled=" << (opts.gc_enabled ? 1 : 0) << '\n';
  std::cout << "clients=" << opts.clients << '\n';
  std::cout << "num_pipes=" << opts.num_pipes << '\n';
  std::cout << "records=" << data.records.size() << '\n';
  std::cout << "transactions=" << data.transactions.size() << '\n';
  std::cout << "reads=" << counts.reads << '\n';
  std::cout << "scans=" << counts.scans << '\n';
  std::cout << "updates=" << counts.updates << '\n';
  std::cout << "ignored_transactions=" << counts.ignored << '\n';
  std::cout << "init_elapsed_seconds=" << init_seconds << '\n';
  std::cout << "elapsed_seconds=" << process_seconds << '\n';
  std::cout << "gc_collect_elapsed_seconds=" << collect_seconds << '\n';
  std::cout << "total_elapsed_seconds="
            << (init_seconds + process_seconds + collect_seconds) << '\n';
  std::cout << "checksum=" << checksum << '\n';
  std::cout << "gc_retired_nodes=" << gc.retired_nodes << '\n';
  std::cout << "gc_reclaimed_nodes=" << gc.reclaimed_nodes << '\n';
  std::cout << "gc_pending_nodes=" << gc.pending_nodes << '\n';
  std::cout << "gc_retired_bytes=" << gc.retired_bytes << '\n';
  std::cout << "gc_reclaimed_bytes=" << gc.reclaimed_bytes << '\n';
  std::cout << "gc_pending_bytes=" << gc.pending_bytes << '\n';
  std::cout << "live_nodes=" << live.nodes << '\n';
  std::cout << "live_internal_nodes=" << live.internal_nodes << '\n';
  std::cout << "live_leaf_nodes=" << live.leaf_nodes << '\n';
  std::cout << "live_sidecar_leaves=" << live.sidecar_leaves << '\n';
  std::cout << "live_delta_records=" << live.delta_records << '\n';
  std::cout << "live_requested_bytes=" << live.requested_bytes << '\n';
  std::cout << "live_sidecar_requested_bytes="
            << live.sidecar_requested_bytes << '\n';
  std::cout << "current_rss_bytes=" << memory.current_rss_bytes << '\n';
  std::cout << "peak_rss_bytes=" << memory.peak_rss_bytes << '\n';
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(parse_options(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "betree_fat_bench: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
