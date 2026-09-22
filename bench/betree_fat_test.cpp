#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include "lib/conctrl/seqcow.hpp"
#include "src/adapters/betree.hpp"

namespace {

using scheduler_type = conctrl::seqcow<betree::interface>;

struct snapshot_view {
  betree::cnodeptr root;
  uint64_t version;
};

struct test_state {
  unsigned failures = 0;

  void expect(bool condition, const char* test, const char* detail) {
    if (condition) { return; }
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s\n", test, detail);
  }
};

snapshot_view snapshot(scheduler_type& scheduler, uint64_t cid = 0) {
  return scheduler.query(cid, [](auto root, uint64_t version) {
    return snapshot_view{root, version};
  });
}

std::optional<uint64_t> find_at(betree::cnodeptr root, uint64_t key,
                                uint64_t version) {
  return betree::interface::find(root, key, version);
}

std::vector<kv> scan_at(betree::cnodeptr root, uint64_t key, uint64_t count,
                        uint64_t version) {
  return betree::interface::scan(root, key, count, version);
}

uint64_t submit(scheduler_type& scheduler, uint64_t key, uint64_t value) {
  const uint64_t tid = scheduler.update(key, value);
  scheduler.wait_for_processing(tid);
  return tid;
}

betree::cnodeptr leaf_for(betree::cnodeptr root, uint64_t key) {
  auto node = root;
  while (node != nullptr && node->type != betree::LEAF) {
    node = node->chs[betree::findch(node, key)];
  }
  return node;
}

std::vector<kv> make_dense_data(uint64_t count) {
  std::vector<kv> data;
  data.reserve(count);
  for (uint64_t key = 0; key < count; ++key) {
    data.emplace_back(key, 1000 + key);
  }
  return data;
}

std::vector<kv> expected_scan(const std::map<uint64_t, uint64_t>& reference,
                              uint64_t lower_bound, uint64_t count) {
  std::vector<kv> expected;
  for (auto it = reference.lower_bound(lower_bound);
       it != reference.end() && expected.size() < count; ++it) {
    expected.emplace_back(it->first, it->second);
  }
  return expected;
}

void test_cli_default_and_explicit_disable(test_state& state) {
  constexpr const char* TEST = "cli-default/explicit-disable";
  char program[] = "main";
  char dataset[] = "data.bin";
  char scheduler[] = "seqcow";
  char structure[] = "betree";
  char fat_option[] = "--fat-slots";
  char zero[] = "0";

  char* default_arguments[] = {program, dataset, scheduler, structure};
  const config default_config = parse_args(4, default_arguments);
  state.expect(default_config.fat_slots == DEFAULT_BETREE_FAT_SLOTS, TEST,
               "seqcow + betree did not select the measured default");

  char* disabled_arguments[] = {
    program, dataset, scheduler, structure, fat_option, zero};
  const config disabled_config = parse_args(6, disabled_arguments);
  state.expect(disabled_config.fat_slots == 0, TEST,
               "an explicit slot zero did not preserve the original path");
}

void test_versions_root_reuse_and_materialization(test_state& state,
                                                   uint8_t capacity) {
  constexpr const char* TEST = "versions/root-reuse/materialization";
  constexpr uint64_t KEY = 10;

  auto data = make_dense_data(32);
  config cfg;
  cfg.fat_slots = capacity;
  auto initial_root = betree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 1, false, capacity);

  const snapshot_view before = snapshot(*scheduler);
  state.expect(before.version == 0, TEST, "initial version is not zero");
  state.expect(find_at(before.root, KEY, before.version) == 1010, TEST,
               "initial value is incorrect");

  std::vector<snapshot_view> versions{before};
  for (uint64_t i = 1; i <= capacity; ++i) {
    const uint64_t tid = submit(*scheduler, KEY, 2000 + i);
    const snapshot_view current = snapshot(*scheduler);
    versions.push_back(current);
    state.expect(current.version == tid, TEST,
                 "published version does not match the update id");
    state.expect(current.root == before.root, TEST,
                 "root changed before the configured slots were full");
    state.expect(find_at(current.root, KEY, current.version) == 2000 + i,
                 TEST, "latest buffered value is not visible");
  }

  const auto buffered_leaf = leaf_for(before.root, KEY);
  state.expect(buffered_leaf != nullptr, TEST, "updated leaf was not found");
  if (buffered_leaf != nullptr) {
    state.expect(betree::leaf_delta_capacity(buffered_leaf) == capacity, TEST,
                 "leaf delta capacity differs from the scheduler capacity");
    state.expect(betree::leaf_delta_count(buffered_leaf) == capacity, TEST,
                 "leaf did not retain exactly N version records");
  }

  state.expect(find_at(before.root, KEY, 0) == 1010, TEST,
               "version zero observed a buffered update");
  for (uint64_t version = 1; version <= capacity; ++version) {
    state.expect(find_at(before.root, KEY, version) == 2000 + version, TEST,
                 "a historical buffered value is incorrect");
  }

  const uint64_t overflow_tid =
    submit(*scheduler, KEY, 2000 + capacity + 1);
  const snapshot_view after = snapshot(*scheduler);
  state.expect(after.version == overflow_tid, TEST,
               "overflow update was not published");
  state.expect(after.root != before.root, TEST,
               "the N+1 update did not materialize and copy the path");
  state.expect(find_at(after.root, KEY, after.version) ==
               2000 + capacity + 1, TEST,
               "materialized leaf does not contain the overflow value");

  const auto materialized_leaf = leaf_for(after.root, KEY);
  state.expect(materialized_leaf != nullptr, TEST,
               "materialized leaf was not found");
  if (materialized_leaf != nullptr) {
    state.expect(betree::leaf_delta_count(materialized_leaf) == 0, TEST,
                 "new materialized leaf retained the old sidecar records");
  }

  /* GC is disabled, so later materialization cannot invalidate these roots. */
  state.expect(find_at(before.root, KEY, 0) == 1010, TEST,
               "materialization damaged the original physical snapshot");
  for (uint64_t version = 1; version <= capacity; ++version) {
    state.expect(find_at(before.root, KEY, version) == 2000 + version, TEST,
                 "materialization damaged a buffered historical snapshot");
  }
}

void test_insert_split_and_scan(test_state& state, uint8_t capacity) {
  constexpr const char* TEST = "insert-split/scan";

  std::vector<kv> data;
  for (uint64_t key = 0; key < 150; key += 10) {
    data.emplace_back(key, 1000 + key);
  }

  config cfg;
  cfg.fat_slots = capacity;
  auto initial_root = betree::interface::build(cfg, data.size(), data.data());
  state.expect(initial_root->type == betree::LEAF, TEST,
               "15 initial entries should form one leaf");
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 1, false, capacity);

  const snapshot_view before = snapshot(*scheduler);
  for (uint64_t key = 1; key <= capacity; ++key) {
    submit(*scheduler, key, 5000 + key);
    state.expect(snapshot(*scheduler).root == before.root, TEST,
                 "an insertion changed the root before sidecar overflow");
  }

  const uint64_t overflow_key = capacity + 1;
  submit(*scheduler, overflow_key, 5000 + overflow_key);
  const snapshot_view after = snapshot(*scheduler);
  state.expect(after.root != before.root, TEST,
               "the N+1 distinct insertion did not materialize");
  state.expect(after.root->type != betree::LEAF, TEST,
               "more than 15 logical entries did not split the full leaf");
  if (after.root->type != betree::LEAF) {
    state.expect(after.root->size == 1, TEST,
                 "leaf split did not create one root separator");
    state.expect(after.root->chs[0]->type == betree::LEAF &&
                 after.root->chs[1]->type == betree::LEAF,
                 TEST, "leaf split did not produce two leaf children");
  }

  std::map<uint64_t, uint64_t> reference;
  for (const auto& [key, value] : data) { reference[key] = value; }
  for (uint64_t key = 1; key <= overflow_key; ++key) {
    reference[key] = 5000 + key;
  }
  state.expect(scan_at(after.root, 0, reference.size() + 4, after.version) ==
               expected_scan(reference, 0, reference.size() + 4), TEST,
               "scan after materialization/split is incomplete or unsorted");
}

void test_scan_deduplicates_versioned_updates(test_state& state) {
  constexpr const char* TEST = "scan-version-filter/deduplicate";
  constexpr uint8_t CAPACITY = 4;

  std::vector<kv> data{{10, 100}, {20, 200}, {30, 300}};
  config cfg;
  cfg.fat_slots = CAPACITY;
  auto initial_root = betree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 1, false, CAPACITY);

  submit(*scheduler, 20, 201);
  submit(*scheduler, 25, 250);
  submit(*scheduler, 20, 202);
  submit(*scheduler, 5, 50);

  const snapshot_view current = snapshot(*scheduler);
  state.expect(current.root == initial_root, TEST,
               "scan setup unexpectedly materialized the leaf");

  const std::vector<std::vector<kv>> expected{
    {{10, 100}, {20, 200}, {30, 300}},
    {{10, 100}, {20, 201}, {30, 300}},
    {{10, 100}, {20, 201}, {25, 250}, {30, 300}},
    {{10, 100}, {20, 202}, {25, 250}, {30, 300}},
    {{5, 50}, {10, 100}, {20, 202}, {25, 250}, {30, 300}}
  };

  for (uint64_t version = 0; version < expected.size(); ++version) {
    state.expect(scan_at(current.root, 0, 16, version) == expected[version],
                 TEST,
                 "scan did not filter, deduplicate, and sort a snapshot");
  }
}

void test_pinned_reader_and_gc(test_state& state, uint8_t capacity) {
  constexpr const char* TEST = "pinned-reader/gc";
  constexpr uint64_t KEY = 10;

  auto data = make_dense_data(64);
  config cfg;
  cfg.fat_slots = capacity;
  auto initial_root = betree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 2, true, capacity);

  std::atomic_bool pinned = false;
  std::atomic_bool release_reader = false;
  betree::cnodeptr reader_root = nullptr;
  uint64_t reader_version = UINT64_MAX;
  std::optional<uint64_t> reader_value;

  std::thread reader([&] {
    reader_value = scheduler->query(0, [&](auto root, uint64_t version) {
      reader_root = root;
      reader_version = version;
      pinned.store(true, std::memory_order_release);
      while (!release_reader.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      return betree::interface::find(root, KEY, version);
    });
  });

  while (!pinned.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  for (uint64_t i = 1; i <= capacity + 1; ++i) {
    submit(*scheduler, KEY, 3000 + i);
  }

  const auto blocked = scheduler->gc_statistics();
  const snapshot_view newest = snapshot(*scheduler, 1);
  state.expect(reader_root == initial_root && reader_version == 0, TEST,
               "reader did not pin the original snapshot");
  state.expect(newest.root != initial_root, TEST,
               "writer did not replace the path after sidecar overflow");
  state.expect(find_at(newest.root, KEY, newest.version) ==
               3000 + capacity + 1, TEST,
               "new reader cannot see the latest materialized value");
  state.expect(blocked.pending_nodes > 0 && blocked.pending_bytes > 0, TEST,
               "GC reclaimed the old path while its reader was pinned");

  release_reader.store(true, std::memory_order_release);
  reader.join();
  state.expect(reader_value == 1010, TEST,
               "old reader observed a later sidecar/materialized value");

  scheduler->collect_garbage();
  const auto released = scheduler->gc_statistics();
  state.expect(released.pending_nodes == 0 && released.pending_bytes == 0,
               TEST, "retired allocations remained after reader exit");
  state.expect(released.retired_nodes == released.reclaimed_nodes &&
               released.retired_bytes == released.reclaimed_bytes,
               TEST, "not all retired allocations were reclaimed");
}

void test_randomized_reference_model(test_state& state, uint8_t capacity) {
  constexpr const char* TEST = "randomized-reference-model";
  constexpr uint64_t KEY_SPACE = 512;
  constexpr uint64_t UPDATE_COUNT = 600;

  std::vector<kv> data;
  std::map<uint64_t, uint64_t> reference;
  for (uint64_t key = 0; key < 30; key += 2) {
    data.emplace_back(key, 1000 + key);
    reference.emplace(key, 1000 + key);
  }

  config cfg;
  cfg.fat_slots = capacity;
  auto initial_root = betree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 1, false, capacity);

  struct saved_snapshot {
    snapshot_view view;
    std::map<uint64_t, uint64_t> reference;
  };
  std::vector<saved_snapshot> history{{snapshot(*scheduler), reference}};

  std::mt19937_64 random(0xbe7ee000ULL + capacity);
  for (uint64_t update = 1; update <= UPDATE_COUNT; ++update) {
    const uint64_t key = random() % KEY_SPACE;
    const uint64_t value = 1000000 + update;
    const uint64_t tid = submit(*scheduler, key, value);
    reference[key] = value;

    const snapshot_view current = snapshot(*scheduler);
    state.expect(current.version == tid, TEST,
                 "published version diverged from the update id");
    state.expect(find_at(current.root, key, current.version) == value, TEST,
                 "point lookup diverged from the reference map");

    const uint64_t lower_bound = random() % KEY_SPACE;
    const uint64_t count = 1 + random() % 24;
    state.expect(scan_at(current.root, lower_bound, count, current.version) ==
                 expected_scan(reference, lower_bound, count), TEST,
                 "range scan diverged from the reference map");

    if (update % 41 == 0) { history.push_back({current, reference}); }
  }

  /* GC is disabled, so saved physical roots remain valid.  Later appends to
   * a shared leaf must nevertheless be hidden by the saved version. */
  for (const saved_snapshot& saved : history) {
    for (uint64_t key = 0; key < KEY_SPACE; ++key) {
      const auto it = saved.reference.find(key);
      const std::optional<uint64_t> expected =
        it == saved.reference.end() ? std::nullopt
                                    : std::make_optional(it->second);
      state.expect(find_at(saved.view.root, key, saved.view.version) == expected,
                   TEST, "historical lookup changed after later writes");
    }
    state.expect(scan_at(saved.view.root, 0, KEY_SPACE, saved.view.version) ==
                 expected_scan(saved.reference, 0, KEY_SPACE), TEST,
                 "historical scan changed after later writes");
  }
}

void test_concurrent_snapshot_readers(test_state& state, uint8_t capacity) {
  constexpr const char* TEST = "concurrent-snapshot-readers";
  constexpr uint64_t READERS = 4;
  constexpr uint64_t KEYS = 8;
  constexpr uint64_t UPDATES = 3000;

  std::vector<kv> data;
  for (uint64_t key = 0; key < KEYS; ++key) {
    data.emplace_back(key, 1000 + key);
  }
  config cfg;
  cfg.fat_slots = capacity;
  auto initial_root = betree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, READERS, true, capacity);

  auto expected_value = [](uint64_t version, uint64_t key) {
    uint64_t latest = version;
    while (latest != 0 && latest % KEYS != key) { --latest; }
    return latest == 0 ? 1000 + key : latest;
  };

  std::atomic_bool start = false;
  std::atomic_bool done = false;
  std::atomic_uint64_t reader_failures = 0;
  std::atomic_uint64_t reader_iterations = 0;
  std::vector<std::thread> readers;
  for (uint64_t cid = 0; cid < READERS; ++cid) {
    readers.emplace_back([&, cid] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      do {
        scheduler->query(cid, [&](auto root, uint64_t version) {
          for (uint64_t key = 0; key < KEYS; ++key) {
            if (betree::interface::find(root, key, version) !=
                expected_value(version, key)) {
              reader_failures.fetch_add(1, std::memory_order_relaxed);
            }
          }

          const auto actual =
            betree::interface::scan(root, 0, KEYS, version);
          if (actual.size() != KEYS) {
            reader_failures.fetch_add(1, std::memory_order_relaxed);
          } else {
            for (uint64_t key = 0; key < KEYS; ++key) {
              if (actual[key] != kv(key, expected_value(version, key))) {
                reader_failures.fetch_add(1, std::memory_order_relaxed);
              }
            }
          }
          reader_iterations.fetch_add(1, std::memory_order_relaxed);
          return 0;
        });
      } while (!done.load(std::memory_order_acquire));
    });
  }

  start.store(true, std::memory_order_release);
  for (uint64_t version = 1; version <= UPDATES; ++version) {
    scheduler->update(version % KEYS, version);
  }
  scheduler->wait_for_processing(UPDATES);
  done.store(true, std::memory_order_release);
  for (std::thread& reader : readers) { reader.join(); }

  state.expect(reader_iterations.load(std::memory_order_relaxed) >= READERS,
               TEST, "reader threads did not inspect a snapshot");
  state.expect(reader_failures.load(std::memory_order_relaxed) == 0,
               TEST, "a reader observed a torn/version-inconsistent tree");

  scheduler->collect_garbage();
  const auto stats = scheduler->gc_statistics();
  state.expect(stats.pending_nodes == 0 && stats.pending_bytes == 0, TEST,
               "GC retained nodes after all readers exited");
  state.expect(stats.retired_nodes == stats.reclaimed_nodes &&
               stats.retired_bytes == stats.reclaimed_bytes,
               TEST, "GC counters did not balance after concurrent reads");
}

void test_full_checkpoint_materializes_sidecars(test_state& state,
                                                uint8_t capacity) {
  constexpr const char* TEST = "full-checkpoint/materializes-sidecars";
  constexpr uint64_t KEY = 10;

  auto data = make_dense_data(64);
  config cfg;
  cfg.num_pipes = 10;
  cfg.fat_slots = capacity;
  auto initial_root = betree::interface::build(cfg, data.size(), data.data());
  betree::nodeptr old_root = initial_root;
  uint64_t version = 0;
  {
    auto scheduler = std::make_unique<scheduler_type>(
      initial_root, 1, false, capacity);
    for (uint64_t i = 1; i <= capacity; ++i) {
      submit(*scheduler, KEY, 7000 + i);
    }
    const snapshot_view current = snapshot(*scheduler);
    old_root = const_cast<betree::nodeptr>(current.root);
    version = current.version;
  }

  state.expect(betree::leaf_over_boundary(old_root, old_root->verge), TEST,
               "test setup did not select a full checkpoint");
  const auto old_leaf = leaf_for(old_root, KEY);
  state.expect(old_leaf != nullptr &&
               betree::leaf_delta_count(old_leaf) == capacity, TEST,
               "source leaf does not contain the expected sidecar records");

  betree::nodeptr rebuilt =
    betree::rebuild(old_root, old_root->verge, version);
  const auto rebuilt_leaf = leaf_for(rebuilt, KEY);
  state.expect(rebuilt != old_root && rebuilt_leaf != old_leaf, TEST,
               "full checkpoint unexpectedly shared the old leaf");
  state.expect(find_at(rebuilt, KEY, version) == 7000 + capacity, TEST,
               "full checkpoint lost the newest visible sidecar value");
  state.expect(rebuilt_leaf != nullptr &&
               betree::leaf_delta_count(rebuilt_leaf) == 0, TEST,
               "full checkpoint did not materialize into a clean leaf");

  /* A full rebuild shares no nodes.  Releasing its source must also release
   * source sidecars without invalidating the rebuilt tree. */
  betree::free_checkpoint(old_root);
  state.expect(find_at(rebuilt, KEY, version) == 7000 + capacity, TEST,
               "freeing the full-checkpoint source damaged the rebuilt tree");
}

void test_full_checkpoint_filters_future_slots(test_state& state) {
  constexpr const char* TEST = "full-checkpoint/version-filter";
  constexpr uint8_t CAPACITY = 4;
  constexpr uint64_t KEY = 10;

  auto data = make_dense_data(15);
  config cfg;
  cfg.num_pipes = 10;
  auto old_root = betree::interface::build(cfg, data.size(), data.data());
  state.expect(old_root->type == betree::LEAF, TEST,
               "filter test requires a single source leaf");
  state.expect(betree::append_leaf_delta(old_root, CAPACITY, 1, KEY, 8001),
               TEST, "could not append the first direct sidecar record");
  state.expect(betree::append_leaf_delta(old_root, CAPACITY, 2, KEY, 8002),
               TEST, "could not append the future direct sidecar record");

  betree::nodeptr rebuilt =
    betree::rebuild(old_root, old_root->verge, 1);
  state.expect(find_at(rebuilt, KEY, 1) == 8001, TEST,
               "checkpoint omitted a record visible at its version");
  state.expect(find_at(rebuilt, KEY, UINT64_MAX) == 8001, TEST,
               "checkpoint materialized a future sidecar record");

  betree::free_checkpoint(old_root);
  state.expect(find_at(rebuilt, KEY, UINT64_MAX) == 8001, TEST,
               "rebuilt result depended on the released source sidecar");
}

void test_partial_checkpoint_preserves_sidecars(test_state& state,
                                                uint8_t capacity) {
  constexpr const char* TEST = "partial-checkpoint/preserves-sidecars";
  constexpr uint64_t RECORDS = 65536;
  constexpr uint64_t KEY = 12345;

  auto data = make_dense_data(RECORDS);
  config cfg;
  cfg.num_pipes = 4;
  cfg.fat_slots = capacity;
  auto initial_root = betree::interface::build(cfg, data.size(), data.data());
  betree::nodeptr old_root = initial_root;
  uint64_t version = 0;
  {
    auto scheduler = std::make_unique<scheduler_type>(
      initial_root, 1, false, capacity);
    for (uint64_t i = 1; i <= capacity; ++i) {
      submit(*scheduler, KEY, 9000 + i);
    }
    const snapshot_view current = snapshot(*scheduler);
    old_root = const_cast<betree::nodeptr>(current.root);
    version = current.version;
  }

  state.expect(!betree::leaf_over_boundary(old_root, old_root->verge), TEST,
               "test setup did not select a partial/upper checkpoint");
  const auto old_leaf = leaf_for(old_root, KEY);
  const auto old_sidecar = betree::leaf_delta(old_leaf);
  state.expect(old_leaf != nullptr && old_sidecar != nullptr &&
               betree::leaf_delta_count(old_leaf) == capacity, TEST,
               "source lower subtree lacks the expected sidecar");

  betree::nodeptr rebuilt =
    betree::rebuild(old_root, old_root->verge, version);
  const auto rebuilt_leaf = leaf_for(rebuilt, KEY);
  state.expect(rebuilt != old_root, TEST,
               "partial checkpoint did not replace its upper root");
  state.expect(rebuilt_leaf == old_leaf, TEST,
               "partial checkpoint copied instead of sharing lower subtree");
  state.expect(betree::leaf_delta(rebuilt_leaf) == old_sidecar, TEST,
               "partial checkpoint lost/replaced the shared leaf sidecar");
  state.expect(find_at(rebuilt, KEY, version) == 9000 + capacity, TEST,
               "partial checkpoint cannot read its shared sidecar value");

  /* free_checkpoint must stop at the shared lower subtrees. */
  betree::free_checkpoint(old_root);
  state.expect(find_at(rebuilt, KEY, version) == 9000 + capacity, TEST,
               "retiring old upper nodes freed a shared leaf/sidecar");
  state.expect(scan_at(rebuilt, KEY, 1, version) ==
               std::vector<kv>{{KEY, 9000 + capacity}}, TEST,
               "scan failed after retiring a partial-checkpoint source");
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  test_state state;
  test_cli_default_and_explicit_disable(state);
  for (uint8_t capacity : {uint8_t{2}, uint8_t{4}, uint8_t{8}}) {
    test_versions_root_reuse_and_materialization(state, capacity);
    test_insert_split_and_scan(state, capacity);
    test_pinned_reader_and_gc(state, capacity);
    test_randomized_reference_model(state, capacity);
    test_concurrent_snapshot_readers(state, capacity);
    test_full_checkpoint_materializes_sidecars(state, capacity);
    test_partial_checkpoint_preserves_sidecars(state, capacity);
  }
  test_scan_deduplicates_versioned_updates(state);
  test_full_checkpoint_filters_future_slots(state);

  if (state.failures != 0) {
    std::fprintf(stderr, "betree fat-node tests: %u failure(s)\n",
                 state.failures);
    return 1;
  }

  std::printf("betree fat-node tests: pass\n");
  return 0;
}
