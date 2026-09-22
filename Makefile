CC = g++
CPPFLAGS = -I. $(MIMALLOC_FLAGS) -lpthread -lmimalloc -march=native -msse4 -maes -std=c++20 -O3 -DNDEBUG
CPPFLAGS_DEBUG = -I. $(MIMALLOC_FLAGS) -lpthread -lmimalloc -march=native -msse4 -maes -std=c++20 -g
EXEC=main
PYTHON ?= python3
GC_BENCH_EXEC ?= bench/betree_gc_bench
# Override MIMALLOC_PREFIX for a nonstandard installation. Auto-detect known
# Homebrew prefixes without invoking brew (which may try a network update).
MIMALLOC_PREFIX ?= $(firstword $(foreach p,$(HOME)/.brew /opt/homebrew /usr/local,$(if $(wildcard $(p)/include/mimalloc.h),$(p))))
MIMALLOC_FLAGS = $(if $(MIMALLOC_PREFIX),-I$(MIMALLOC_PREFIX)/include -L$(MIMALLOC_PREFIX)/lib -Wl$(comma)-rpath$(comma)$(MIMALLOC_PREFIX)/lib)
comma := ,
GC_BENCH_FLAGS ?= -I. $(MIMALLOC_FLAGS) -lpthread -lmimalloc -std=c++20 -O3 -DNDEBUG
FAT_TEST_EXEC ?= bench/btree_fat_test
FAT_TEST_FLAGS ?= $(GC_BENCH_FLAGS)
FAT_BENCH_EXEC ?= bench/btree_fat_bench
FAT_BENCH_FLAGS ?= $(GC_BENCH_FLAGS)
BETREE_FAT_TEST_EXEC ?= bench/betree_fat_test
BETREE_FAT_TEST_FLAGS ?= $(GC_BENCH_FLAGS)
BETREE_FAT_BENCH_EXEC ?= bench/betree_fat_bench
BETREE_FAT_BENCH_FLAGS ?= $(GC_BENCH_FLAGS)

BENCH_SCHEDULER ?= seqcow
BENCH_STRUCTURE ?= art
BENCH_CLIENTS ?= 1
BENCH_PIPES ?= 1
BENCH_WORKERS ?= 1
BENCH_RUNS ?= 3
BENCH_WARMUPS ?= 0
BENCH_OUTPUT ?=
BENCH_OUTPUT_OPT = $(if $(strip $(BENCH_OUTPUT)),--output "$(BENCH_OUTPUT)",)

all: $(EXEC)

$(EXEC): src/main.cpp
	$(CC) $< $(CPPFLAGS) -o $(EXEC)

gen:
	@make -C ycsbc gen

bench-memory: $(EXEC)
	$(PYTHON) bench/memory_bench.py \
		--binary ./$(EXEC) \
		--dataset "$(DATASET)" \
		--scheduler "$(BENCH_SCHEDULER)" \
		--structure "$(BENCH_STRUCTURE)" \
		--clients "$(BENCH_CLIENTS)" \
		--pipes "$(BENCH_PIPES)" \
		--workers "$(BENCH_WORKERS)" \
		--runs "$(BENCH_RUNS)" \
		--warmups "$(BENCH_WARMUPS)" \
		$(BENCH_OUTPUT_OPT) -- $(BENCH_ARGS)

$(GC_BENCH_EXEC): bench/betree_gc_bench.cpp
	$(CC) $< -o $@ $(GC_BENCH_FLAGS)

bench-gc-native: $(GC_BENCH_EXEC)

$(FAT_TEST_EXEC): bench/btree_fat_test.cpp
	$(CC) $< -o $@ $(FAT_TEST_FLAGS)

test-fat-btree: $(FAT_TEST_EXEC)
	$<

$(FAT_BENCH_EXEC): bench/btree_fat_bench.cpp
	$(CC) $< -o $@ $(FAT_BENCH_FLAGS)

bench-fat-btree: $(FAT_BENCH_EXEC)

$(BETREE_FAT_TEST_EXEC): bench/betree_fat_test.cpp
	$(CC) $< -o $@ $(BETREE_FAT_TEST_FLAGS)

test-fat-betree: $(BETREE_FAT_TEST_EXEC)
	$<

$(BETREE_FAT_BENCH_EXEC): bench/betree_fat_bench.cpp
	$(CC) $< -o $@ $(BETREE_FAT_BENCH_FLAGS)

bench-fat-betree: $(BETREE_FAT_BENCH_EXEC)

clean:
	@$(RM) bench/concow_aert_test bench/concow_aert_bench \
		bench/concow_betree_test bench/concow_betree_bench
	@$(RM) $(EXEC) $(GC_BENCH_EXEC) $(FAT_TEST_EXEC) $(FAT_BENCH_EXEC) \
		$(BETREE_FAT_TEST_EXEC) $(BETREE_FAT_BENCH_EXEC) bench/radix_fat_test bench/art_fat_bench bench/aert_fat_bench \
		bench/concow_fat_test bench/concow_fat_bench bench/concow_fat_profile \
		bench/concow_cyclic_fat_test bench/concow_cyclic_fat_bench \
		bench/concow_cyclic_fat_p1_bench \
		bench/concow_progress_test \
		bench/concow_cyclic_gc_test bench/concow_worker_fat_test \
		bench/native_betree_fat_test bench/native_art_fat_test bench/native_aert_fat_test bench/native_ticket_wait_test \
		bench/concow_art_test bench/concow_art_bench

.PHONY: all clean bench-memory bench-gc-native test-fat-btree bench-fat-btree \
	test-fat-betree bench-fat-betree

RADIX_FAT_FLAGS ?= $(GC_BENCH_FLAGS)
RADIX_HEADERS := $(wildcard lib/trees/art/*.hpp lib/trees/aert/*.hpp lib/common/*.hpp lib/conctrl/*.hpp src/adapters/*.hpp) src/config.hpp bench/radix_walk.hpp
bench/radix_fat_test: bench/radix_fat_test.cpp $(RADIX_HEADERS)
	$(CC) $< -o $@ $(RADIX_FAT_FLAGS)
test-fat-radix: bench/radix_fat_test
	$<
bench/art_fat_bench: bench/radix_fat_bench.cpp $(RADIX_HEADERS)
	$(CC) $< -o $@ $(RADIX_FAT_FLAGS)
bench/aert_fat_bench: bench/radix_fat_bench.cpp $(RADIX_HEADERS)
	$(CC) $< -o $@ $(RADIX_FAT_FLAGS) -DRADIX_AERT
bench-fat-radix: bench/art_fat_bench bench/aert_fat_bench
.PHONY: test-fat-radix bench-fat-radix

CONCOW_FAT_HEADERS := $(wildcard lib/trees/btree/*.hpp lib/trees/art/*.hpp \
	lib/trees/aert/*.hpp lib/trees/betree/*.hpp lib/common/*.hpp \
	lib/conctrl/*.hpp src/adapters/*.hpp) src/config.hpp
CONCOW_FAT_FLAGS ?= $(GC_BENCH_FLAGS)
bench/concow_fat_test: bench/concow_fat_test.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS)
test-concow-fat: bench/concow_fat_test
	$<
.PHONY: test-concow-fat
bench/concow_fat_bench: bench/concow_fat_bench.cpp $(CONCOW_FAT_HEADERS) utils/memory_stats.hpp
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS)
bench-concow-fat: bench/concow_fat_bench

bench/concow_fat_profile: bench/concow_fat_bench.cpp $(CONCOW_FAT_HEADERS) utils/memory_stats.hpp
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DCONCOW_FAT_PROFILE=1
profile-concow-fat: bench/concow_fat_profile

.PHONY: profile-concow-fat

bench/concow_cyclic_fat_test: bench/concow_cyclic_fat_test.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DLIBCONCTRL_BUFFER_SIZE=1024
test-concow-cyclic-fat: bench/concow_cyclic_fat_test
	bench/concow_cyclic_fat_test
.PHONY: test-concow-cyclic-fat

bench/concow_progress_test: bench/concow_progress_test.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DLIBCONCTRL_BUFFER_SIZE=1024
test-concow-progress: bench/concow_progress_test
	bench/concow_progress_test
.PHONY: test-concow-progress

bench/concow_cyclic_gc_test: bench/concow_cyclic_gc_test.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DLIBCONCTRL_BUFFER_SIZE=1024
test-concow-cyclic-gc: bench/concow_cyclic_gc_test
	bench/concow_cyclic_gc_test
.PHONY: test-concow-cyclic-gc

bench/concow_cyclic_fat_bench: bench/concow_fat_bench.cpp $(CONCOW_FAT_HEADERS) utils/memory_stats.hpp
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DCONCOW_NATIVE=1
bench-concow-cyclic-fat: bench/concow_cyclic_fat_bench
.PHONY: bench-concow-cyclic-fat
bench/concow_cyclic_fat_p1_bench: bench/concow_fat_bench.cpp $(CONCOW_FAT_HEADERS) utils/memory_stats.hpp
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DCONCOW_NATIVE=1 -DCONCOW_NATIVE_PIPES=1
bench-concow-cyclic-fat-p1: bench/concow_cyclic_fat_p1_bench
.PHONY: bench-concow-cyclic-fat-p1
.PHONY: bench-concow-fat

bench/concow_art_test: bench/concow_art_test.cpp $(CONCOW_FAT_HEADERS) $(RADIX_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS)
test-concow-art: bench/concow_art_test
	bench/concow_art_test
.PHONY: test-concow-art

bench/concow_art_bench: bench/concow_art_bench.cpp $(CONCOW_FAT_HEADERS) $(RADIX_HEADERS) utils/memory_stats.hpp
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS)
bench-concow-art: bench/concow_art_bench
.PHONY: bench-concow-art

bench/concow_aert_test: bench/concow_aert_test.cpp $(CONCOW_FAT_HEADERS) $(RADIX_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS)
test-concow-aert: bench/concow_aert_test
	bench/concow_aert_test
.PHONY: test-concow-aert

bench/concow_aert_bench: bench/concow_aert_bench.cpp $(CONCOW_FAT_HEADERS) $(RADIX_HEADERS) utils/memory_stats.hpp
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS)
bench-concow-aert: bench/concow_aert_bench
.PHONY: bench-concow-aert

bench/concow_betree_test: bench/concow_betree_test.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS)
test-concow-betree: bench/concow_betree_test
	bench/concow_betree_test
.PHONY: test-concow-betree

bench/concow_betree_bench: bench/concow_betree_bench.cpp $(CONCOW_FAT_HEADERS) utils/memory_stats.hpp
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS)
bench-concow-betree: bench/concow_betree_bench
.PHONY: bench-concow-betree

bench/concow_worker_fat_test: bench/concow_worker_fat_test.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DLIBCONCTRL_BUFFER_SIZE=1024
test-concow-worker-fat: bench/concow_worker_fat_test
	bench/concow_worker_fat_test
.PHONY: test-concow-worker-fat

bench/native_betree_fat_test: bench/concow_native_multitree_test.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DTREE_BETREE -DLIBCONCTRL_BUFFER_SIZE=1024
bench/native_art_fat_test: bench/concow_native_multitree_test.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DTREE_ART -DLIBCONCTRL_BUFFER_SIZE=1024
bench/native_aert_fat_test: bench/concow_native_multitree_test.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS) -DTREE_AERT -DLIBCONCTRL_BUFFER_SIZE=1024
test-native-multitree-fat: bench/native_betree_fat_test bench/native_art_fat_test bench/native_aert_fat_test
	bench/native_betree_fat_test
	bench/native_art_fat_test
	bench/native_aert_fat_test
.PHONY: test-native-multitree-fat

bench/native_ticket_wait_test: bench/native_ticket_wait_test.cpp src/main.cpp $(CONCOW_FAT_HEADERS)
	$(CC) $< -o $@ $(CONCOW_FAT_FLAGS)
test-native-ticket-wait: bench/native_ticket_wait_test
	bench/native_ticket_wait_test
.PHONY: test-native-ticket-wait
