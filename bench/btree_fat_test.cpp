#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lib/conctrl/seqcow.hpp"
#include "src/adapters/btree.hpp"

namespace {

/* Keep a four-record case for the focused materialization scenarios.  The
 * configured default is tested independently for every value it may take. */
constexpr uint8_t FAT_SLOTS = 4;

using scheduler_type = conctrl::seqcow<btree::interface>;

struct snapshot_view {
  btree::cnodeptr root;
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

std::optional<uint64_t> find_at(btree::cnodeptr root, uint64_t key,
                                uint64_t version) {
  return btree::interface::find(root, key, version);
}

std::vector<kv> scan_at(btree::cnodeptr root, uint64_t key, uint64_t count,
                        uint64_t version) {
  return btree::interface::scan(root, key, count, version);
}

uint64_t submit(scheduler_type& scheduler, uint64_t key, uint64_t value) {
  const uint64_t tid = scheduler.update(key, value);
  scheduler.wait_for_processing(tid);
  return tid;
}

btree::cnodeptr leaf_for(btree::cnodeptr root, uint64_t key) {
  auto node = root;
  while (node != nullptr && node->type != btree::LEAF) {
    node = node->chs[btree::findch(node, key)];
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

config parse_test_args(std::initializer_list<const char*> arguments) {
  std::vector<std::string> storage(arguments.begin(), arguments.end());
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& argument : storage) { argv.push_back(argument.data()); }
  return parse_args(static_cast<int>(argv.size()), argv.data());
}

void test_scoped_default_capacity(test_state& state) {
  constexpr const char* TEST = "scoped-default-capacity";

  const config default_btree =
    parse_test_args({"main", "dataset", "seqcow", "btree"});
  state.expect(default_btree.fat_slots == DEFAULT_BTREE_FAT_SLOTS, TEST,
               "seqcow + btree did not select the tuned default");

  const config disabled = parse_test_args(
    {"main", "dataset", "seqcow", "btree", "--fat-slots", "0"});
  state.expect(disabled.fat_slots == 0, TEST,
               "an explicit zero did not restore the original path");

  const config explicit_two = parse_test_args(
    {"main", "dataset", "seqcow", "btree", "--fat-slots", "2"});
  state.expect(explicit_two.fat_slots == 2, TEST,
               "an explicit capacity was replaced by the default");

  const config other_tree =
    parse_test_args({"main", "dataset", "seqcow", "art"});
  state.expect(other_tree.fat_slots == 0, TEST,
               "the B+Tree default leaked into another structure");

  const config other_scheduler =
    parse_test_args({"main", "dataset", "concow", "btree"});
  state.expect(other_scheduler.fat_slots == 0, TEST,
               "the SeqCow default leaked into another scheduler");

  const config ordered_wave =
    parse_test_args({"main", "dataset", "concow-fat", "art"});
  state.expect(ordered_wave.scheduler == SC_CONCOW_FAT &&
               ordered_wave.fat_slots == 2, TEST,
               "concow-fat did not select its memory-first default");
  const config ordered_wave_disabled = parse_test_args(
    {"main", "dataset", "concow-fat", "betree", "--fat-slots", "0"});
  state.expect(ordered_wave_disabled.fat_slots == 0, TEST,
               "concow-fat ignored an explicit zero capacity");
}

void test_versions_root_reuse_and_materialization(test_state& state) {
  constexpr const char* TEST = "versions/root-reuse/materialization";
  constexpr uint64_t KEY = 10;

  auto data = make_dense_data(32);
  config cfg;
  cfg.fat_slots = FAT_SLOTS;
  auto initial_root = btree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 1, false, FAT_SLOTS);

  const snapshot_view before = snapshot(*scheduler);
  state.expect(before.version == 0, TEST, "initial snapshot version is not zero");
  state.expect(find_at(before.root, KEY, before.version) == 1010, TEST,
               "initial value is incorrect");

  std::vector<snapshot_view> versions;
  versions.push_back(before);
  for (uint64_t i = 1; i <= FAT_SLOTS; ++i) {
    const uint64_t tid = submit(*scheduler, KEY, 2000 + i);
    const snapshot_view current = snapshot(*scheduler);
    versions.push_back(current);
    state.expect(current.version == tid, TEST,
                 "published version does not match submitted update");
    state.expect(current.root == before.root, TEST,
                 "root changed before the four slots were full");
    state.expect(find_at(current.root, KEY, current.version) == 2000 + i,
                 TEST, "latest value is not visible from its version");
  }

  const auto buffered_leaf = leaf_for(before.root, KEY);
  state.expect(buffered_leaf != nullptr, TEST, "updated leaf was not found");
  if (buffered_leaf != nullptr) {
    state.expect(btree::leaf_delta_capacity(buffered_leaf) == FAT_SLOTS, TEST,
                 "leaf delta capacity is not four");
    state.expect(btree::leaf_delta_count(buffered_leaf) == FAT_SLOTS, TEST,
                 "leaf did not retain four version records");
  }

  const uint64_t expected_values[] = {1010, 2001, 2002, 2003, 2004};
  for (uint64_t version = 0; version <= FAT_SLOTS; ++version) {
    state.expect(find_at(before.root, KEY, version) == expected_values[version],
                 TEST, "a historical value is not selected by snapshot version");
  }

  const uint64_t fifth_tid = submit(*scheduler, KEY, 2005);
  const snapshot_view after = snapshot(*scheduler);
  state.expect(after.version == fifth_tid, TEST,
               "fifth update was not published");
  state.expect(after.root != before.root, TEST,
               "fifth update did not materialize and copy the path");
  state.expect(find_at(after.root, KEY, after.version) == 2005, TEST,
               "materialized tree does not contain the fifth value");

  const auto materialized_leaf = leaf_for(after.root, KEY);
  state.expect(materialized_leaf != nullptr, TEST,
               "materialized leaf was not found");
  if (materialized_leaf != nullptr) {
    state.expect(btree::leaf_delta_count(materialized_leaf) == 0, TEST,
                 "new materialized leaf unexpectedly retained old deltas");
  }

  /* GC is disabled in this case, so the old root remains safe to inspect. */
  for (uint64_t version = 0; version <= FAT_SLOTS; ++version) {
    state.expect(find_at(before.root, KEY, version) == expected_values[version],
                 TEST, "materialization damaged an older physical snapshot");
  }
}

void test_insert_split_and_scan(test_state& state) {
  constexpr const char* TEST = "insert-split/scan";

  std::vector<kv> data;
  for (uint64_t key = 0; key < 150; key += 10) {
    data.emplace_back(key, 1000 + key);
  }

  config cfg;
  cfg.fat_slots = FAT_SLOTS;
  auto initial_root = btree::interface::build(cfg, data.size(), data.data());
  state.expect(initial_root->type == btree::LEAF, TEST,
               "15 initial entries should form one leaf");
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 1, false, FAT_SLOTS);

  const snapshot_view before = snapshot(*scheduler);
  for (uint64_t key = 1; key <= FAT_SLOTS; ++key) {
    submit(*scheduler, key, 5000 + key);
    state.expect(snapshot(*scheduler).root == before.root, TEST,
                 "an insertion changed the root before slot overflow");
  }

  submit(*scheduler, 5, 5005);
  const snapshot_view after = snapshot(*scheduler);
  state.expect(after.root != before.root, TEST,
               "the fifth distinct insertion did not materialize");
  state.expect(after.root->type == btree::INTERNAL, TEST,
               "20 logical entries did not split the original full leaf");
  if (after.root->type == btree::INTERNAL) {
    state.expect(after.root->size == 1, TEST,
                 "leaf split did not create one root separator");
    state.expect(after.root->chs[0]->type == btree::LEAF &&
                 after.root->chs[1]->type == btree::LEAF,
                 TEST, "leaf split did not produce two leaf children");
  }

  std::vector<kv> expected = data;
  for (uint64_t key = 1; key <= 5; ++key) {
    expected.emplace_back(key, 5000 + key);
  }
  std::sort(expected.begin(), expected.end());
  state.expect(scan_at(after.root, 0, expected.size() + 4, after.version) ==
               expected, TEST,
               "scan after materialization/split is not complete and sorted");
}

void test_scan_deduplicates_versioned_updates(test_state& state) {
  constexpr const char* TEST = "scan-version-filter/deduplicate";

  std::vector<kv> data{{10, 100}, {20, 200}, {30, 300}};
  config cfg;
  cfg.fat_slots = FAT_SLOTS;
  auto initial_root = btree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 1, false, FAT_SLOTS);

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

  auto data = make_dense_data(32);
  config cfg;
  cfg.fat_slots = capacity;
  auto initial_root = btree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 2, true, capacity);

  std::atomic_bool pinned = false;
  std::atomic_bool release_reader = false;
  btree::cnodeptr reader_root = nullptr;
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
      return btree::interface::find(root, KEY, version);
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
               "writer did not overflow and replace the pinned path");
  state.expect(find_at(newest.root, KEY, newest.version) ==
               3000 + capacity + 1, TEST,
               "new reader cannot see the writer's latest value");
  state.expect(blocked.pending_nodes > 0, TEST,
               "GC reclaimed the old path while its reader was pinned");
  state.expect(blocked.pending_bytes > 0, TEST,
               "GC did not account for the pinned retired allocation");

  release_reader.store(true, std::memory_order_release);
  reader.join();
  state.expect(reader_value == 1010, TEST,
               "old reader observed a later slot or materialized value");

  scheduler->collect_garbage();
  const auto released = scheduler->gc_statistics();
  state.expect(released.pending_nodes == 0, TEST,
               "retired nodes remained pending after the reader exited");
  state.expect(released.pending_bytes == 0, TEST,
               "retired bytes remained pending after the reader exited");
  state.expect(released.retired_nodes == released.reclaimed_nodes, TEST,
               "not all retired nodes were reclaimed");
  state.expect(released.retired_bytes == released.reclaimed_bytes, TEST,
               "not all retired bytes were reclaimed");
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
  auto initial_root = btree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 1, false, capacity);

  struct saved_snapshot {
    snapshot_view view;
    std::map<uint64_t, uint64_t> reference;
  };
  std::vector<saved_snapshot> history;
  history.push_back({snapshot(*scheduler), reference});

  std::mt19937_64 random(0x5eed0000ULL + capacity);
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

    if (update % 41 == 0) {
      history.push_back({current, reference});
    }
  }

  /* GC is off, so every saved physical root remains available.  Later slot
   * appends must still be hidden by each saved snapshot version. */
  for (const saved_snapshot& saved : history) {
    for (uint64_t key = 0; key < KEY_SPACE; ++key) {
      const auto it = saved.reference.find(key);
      const std::optional<uint64_t> expected =
        it == saved.reference.end() ? std::nullopt
                                    : std::make_optional(it->second);
      state.expect(find_at(saved.view.root, key, saved.view.version) == expected,
                   TEST, "historical point lookup changed after later writes");
    }
    state.expect(scan_at(saved.view.root, 0, KEY_SPACE, saved.view.version) ==
                 expected_scan(saved.reference, 0, KEY_SPACE), TEST,
                 "historical scan changed after later writes");
  }
}

void test_multiclient_registration(test_state& state) {
  constexpr const char* TEST = "multiclient-registration";
  constexpr uint64_t CLIENTS = 8;
  constexpr uint64_t UPDATES_PER_CLIENT = 100;
  constexpr uint64_t TOTAL_UPDATES = CLIENTS * UPDATES_PER_CLIENT;

  std::vector<kv> data{{0, 1000}};
  config cfg;
  cfg.fat_slots = FAT_SLOTS;
  auto initial_root = btree::interface::build(cfg, data.size(), data.data());
  auto scheduler = std::make_unique<scheduler_type>(
    initial_root, 1, true, FAT_SLOTS);

  std::vector<uint64_t> tids(TOTAL_UPDATES);
  std::vector<std::thread> clients;
  for (uint64_t cid = 0; cid < CLIENTS; ++cid) {
    clients.emplace_back([&, cid] {
      for (uint64_t i = 0; i < UPDATES_PER_CLIENT; ++i) {
        const uint64_t index = cid*UPDATES_PER_CLIENT + i;
        const uint64_t key = index + 1;
        tids[index] = scheduler->update(key, 500000 + key);
      }
    });
  }
  for (std::thread& client : clients) { client.join(); }

  std::vector<uint64_t> sorted_tids = tids;
  std::sort(sorted_tids.begin(), sorted_tids.end());
  for (uint64_t i = 0; i < TOTAL_UPDATES; ++i) {
    state.expect(sorted_tids[i] == i + 1, TEST,
                 "client registrations did not form one contiguous frontier");
  }

  scheduler->wait_for_processing(TOTAL_UPDATES);
  const snapshot_view current = snapshot(*scheduler);
  state.expect(current.version == TOTAL_UPDATES, TEST,
               "worker did not commit every registered update");
  for (uint64_t key = 1; key <= TOTAL_UPDATES; ++key) {
    state.expect(find_at(current.root, key, current.version) == 500000 + key,
                 TEST, "a concurrently registered update was lost");
  }

  scheduler->collect_garbage();
  const auto stats = scheduler->gc_statistics();
  state.expect(stats.pending_nodes == 0 && stats.pending_bytes == 0, TEST,
               "GC retained an unpinned path after concurrent registration");
}

void test_concurrent_snapshot_readers(test_state& state, uint8_t capacity) {
  constexpr const char* TEST = "concurrent-snapshot-readers";
  constexpr uint64_t READERS = 4;
  constexpr uint64_t KEYS = 8;
  constexpr uint64_t UPDATES = 4000;

  std::vector<kv> data;
  for (uint64_t key = 0; key < KEYS; ++key) {
    data.emplace_back(key, 1000 + key);
  }
  config cfg;
  cfg.fat_slots = capacity;
  auto initial_root = btree::interface::build(cfg, data.size(), data.data());
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
            if (btree::interface::find(root, key, version) !=
                expected_value(version, key)) {
              reader_failures.fetch_add(1, std::memory_order_relaxed);
            }
          }

          const auto actual = btree::interface::scan(root, 0, KEYS, version);
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
               TEST, "a reader observed a torn or version-inconsistent tree");

  scheduler->collect_garbage();
  const auto stats = scheduler->gc_statistics();
  state.expect(stats.pending_nodes == 0 && stats.pending_bytes == 0, TEST,
               "GC retained nodes after all concurrent readers exited");
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  test_state state;
  test_scoped_default_capacity(state);
  test_versions_root_reuse_and_materialization(state);
  test_insert_split_and_scan(state);
  test_scan_deduplicates_versioned_updates(state);
  test_pinned_reader_and_gc(state, 2);
  test_pinned_reader_and_gc(state, 4);
  test_pinned_reader_and_gc(state, 8);
  test_randomized_reference_model(state, 2);
  test_randomized_reference_model(state, 4);
  test_randomized_reference_model(state, 8);
  test_multiclient_registration(state);
  test_concurrent_snapshot_readers(state, 2);
  test_concurrent_snapshot_readers(state, 4);
  test_concurrent_snapshot_readers(state, 8);

  if (state.failures != 0) {
    std::fprintf(stderr, "btree fat-node tests: %u failure(s)\n",
                 state.failures);
    return 1;
  }

  std::printf("btree fat-node tests: pass\n");
  return 0;
}
