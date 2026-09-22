# ART / AERT fat-node 实现与内存实验

## 设计

范围：补齐 B+Tree 之外的 BeTree、ART、AERT。BeTree 已有实现和正式本地矩阵，
本次保留其实现并运行回归测试；新增 ART/AERT 的 SeqCow fat leaf。

两棵 radix 树的叶节点原本均为 64 bytes，其中 32 bytes 是尾部填充。
新布局在填充区放置两个 uint64 value、两个 uint32 version、一个 overflow 指针。
静态断言保证叶节点仍为 64 bytes。2 槽无需额外分配；4/8 槽直到第三次更新
才分配 overflow，分别请求 40/104 bytes。没有常驻哈希表或每节点锁。

handle_init 先按原树路由找到已有 key 的叶子；槽未满就追加记录并保留 root。
第 N+1 次更新走原 COW；node_copy 仅复制固定头部，读取最新可见 value 到新叶，
清空新叶的槽，绝不复制 sidecar 指针。prefix fork 使用同样的物化操作。
AERT 路由时去掉低 5-bit tag，保留原结构更新中的 tag 编码；共享旧叶时也共享
完整的历史记录，无需改变其 tag。ART 的 fork 保留完整叶 key，避免扫描丢失高位。

写入 payload 后 release 发布 version，reader acquire 读取 version 后才读取 payload。
overflow 指针也使用 release/acquire；记录只追加、发布后不改写。点查和范围扫描
都显式传递 snapshot version。只支持 SeqCow 单 writer；32-bit 版本用尽时提交端
抛出 overflow_error，避免序号回绕让新记录被旧快照误读。ConCow 仍不允许启用。
node_size 包含 overflow，free_node 一起释放；内联记录不重复计费。

## 验证

`make test-fat-radix` 覆盖 2/4/8 槽、历史点查/扫描、N+1 次复制、延迟分配、
全 leading-byte 插入、随机 prefix fork/扩张、参考 map、tagged AERT、pinned reader、
并发 reader 的 point/scan 与 version 一致性。测试释放保留的历史节点及当前树。
普通优化构建和 UBSan 通过；B+Tree、BeTree 现有测试另行回归。
ASan 在本机运行时停在 main 前（放宽 sandbox 后亦然），TSan 在 main 前退出 139；
独立的仅打印消息的最小程序同样出现 ASan 启动超时、TSan SIGSEGV，
因此不声称这两项验证通过。macOS ASan 不支持 detect_leaks=1。
为 ARM64 验证添加 Node16 标量查询分支；x86 保留 SIMD equality，只有具备
AVX512BW/VL 时使用原 SIMD unsigned greater-than，否则使用标量比较。

## 本次测量边界

2026-09-07，ARM64/macOS，Apple clang 17.0.0，-O3 -DNDEBUG，系统分配器。
更正：当时未发现已安装在 `~/.brew` 的 mimalloc，并非本机未安装。
SeqCow、GC on、1 client；每棵树每个场景独立进程，warm-up 1 次、测量 3 次，
沿用 runner 的轮换顺序。10,000 初始 key；uniform/hot/lookup90 各 50,000 操作，
long 为 250,000 操作。hot 是 90% 命中 100 个 key 的热点模型，不冒充 Zipfian。
原始 CSV 仅保存测量记录，warmup 不进入下表均值。96 条测量记录均为
GC pending nodes/bytes = 0。这是小规模分配器实验，不能替代 mimalloc
大规模矩阵，也不能与已有 BeTree mimalloc RSS 数字直接比较。
slot 0 沿用既有 SeqCow 提交路径，因此仅作内存参照，不能把读多写少 checksum
当成与 fat mode 完全一致的并发语义证据。

峰值 RSS / 最终 reachable requested / 累计 retired，单位 MiB：

| 树 | 场景 | slots 0 | slots 2 | slots 4 | slots 8 |
|---|---|---:|---:|---:|---:|
| ART | uniform | 126.844 / 0.690 / 137.201 | 46.104 / 0.690 / 36.647 | 29.469 / 0.851 / 16.398 | 14.536 / 1.498 / 1.930 |
| ART | hot | 126.911 / 0.690 / 137.318 | 42.250 / 0.690 / 41.534 | 32.729 / 0.697 / 24.960 | 23.635 / 0.711 / 14.109 |
| ART | lookup90 | 20.422 / 0.690 / 13.660 | 11.901 / 0.690 / 0.417 | 11.807 / 0.696 / 0.003 | 11.755 / 0.705 / 0.000 |
| ART | long | 132.130 / 0.690 / 685.906 | 55.583 / 0.690 / 219.494 | 40.885 / 0.840 / 128.038 | 33.172 / 1.360 / 66.235 |
| AERT | uniform | 43.302 / 0.733 / 33.557 | 20.771 / 0.733 / 8.963 | 17.094 / 0.894 / 4.180 | 13.344 / 1.541 / 0.523 |
| AERT | hot | 43.240 / 0.733 / 33.568 | 19.589 / 0.733 / 10.153 | 17.771 / 0.740 / 6.360 | 15.333 / 0.754 / 3.820 |
| AERT | lookup90 | 12.177 / 0.733 / 3.341 | 11.880 / 0.733 / 0.102 | 11.823 / 0.738 / 0.001 | 11.802 / 0.748 / 0.000 |
| AERT | long | 48.495 / 0.733 / 167.773 | 27.505 / 0.733 / 53.688 | 23.859 / 0.883 / 32.645 | 22.172 / 1.403 / 17.946 |

2 槽在两棵树所有场景中均与 slot 0 保持相同 reachable bytes，且无外部 sidecar。
4/8 槽在本次写密集场景中通常有更低峰值，但会增加最终树内存。
建议内存优先使用显式 `--fat-slots 2`；ART/AERT 的 CLI 默认暂保留 0，等待
mimalloc、大规模 Uniform/Zipfian/长写入和完整 sanitizer 验证后独立决定默认值。
BeTree 继续使用已有实验支持的默认 2。

## 复现

安装 mimalloc 的机器：`make test-fat-radix bench-fat-radix`。
无 mimalloc 的验证构建（仅 bench，生产路径仍使用 mimalloc）：

```sh
make test-fat-radix bench-fat-radix CC=clang++ \
  RADIX_FAT_FLAGS='-I. -Ibench/system_allocator -std=c++20 -pthread -O3 -DNDEBUG'
python3 bench/generate_radix_workloads.py /tmp/radix-data
python3 bench/fat_slots_bench.py ./bench/art_fat_bench \
  /tmp/radix-data/radix_uniform.data /tmp/radix-data/radix_hot.data \
  /tmp/radix-data/radix_lookup90.data /tmp/radix-data/radix_long.data \
  --runs 3 --warmups 1 --clients 1 --gc --output bench/results/art_fat.csv
```

AERT 将驱动替换为 `./bench/aert_fat_bench`。切换 sanitizer/编译参数前删除旧的
bench 可执行文件以强制重编译。UBSan 加 `-fsanitize=undefined`；在支持的环境
继续补跑 `-fsanitize=address,undefined` 以及单独 `-fsanitize=thread`。

## 后续：真实 mimalloc 验证已完成

已找到本机 mimalloc 3.5.0，修复真实 override 宏兼容问题，并完成
100k 四场景及 1m Uniform 共 120 条测量记录。见
[Mac 环境修复和 mimalloc 实验报告](mac_mimalloc_validation.md)。
本报告原系统分配器测量保留作为历史对照。
