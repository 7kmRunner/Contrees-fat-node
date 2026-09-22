# Mac mimalloc 环境修复及实验（2026-09-07）

## 环境与修复

本机已有 `~/.brew/opt/mimalloc` 3.5.0（ARM64），不是未安装；此前只检查常见前缀
导致误判。此次复用已有安装，没有替换其版本。Makefile 自动检测 `~/.brew`、
`/opt/homebrew`、`/usr/local`，也可用 `MIMALLOC_PREFIX` 指定前缀。

真实 mimalloc 的 override 宏暴露了 fat_value 中 `std::calloc/free` 的编译错误，
现改用全局命名空间调用以与仓库的 mimalloc 宏保持一致。独立探针确认版本
30500、树节点和 overflow 均属于 mimalloc heap，叶节点大小仍为 64 bytes。
`otool -L bench/art_fat_bench` 确认链接 `~/.brew/opt/mimalloc/lib/libmimalloc.3.dylib`。

macOS 26.6.2 (25G83)、Apple clang 17.0.0 (clang-1700.6.4.2)、ARM64、
`-std=c++20 -O3 -DNDEBUG`、SeqCow、GC on、1 client、默认 mimalloc 参数。
完整测试：ART/AERT 的 slots 0/2/4/8 测试、B+Tree/BeTree 回归全部通过。
真实 mimalloc 下 ART/AERT UBSan 也通过。

## 测量

每个样本独立进程；每场景 slots 0/2/4/8，warmup 1 次、测量 3 次，沿用 runner
的轮换顺序。100k：100,000 初始记录，uniform/hot/lookup90 各 500,000 操作，
long 为 2,500,000 操作。1m：1,000,000 初始记录、1,000,000 uniform 更新。
hot 为 90% 命中 100 keys，并非 Zipfian；lookup90 是 90% 读取、10% 已有 key 更新。

下表为峰值 RSS / reachable requested / 累计退休字节，均值，MiB。
原始 CSV 保留时间、GC、读写计数和运行顺序，共 120 条测量记录，GC pending
nodes/bytes 全部为 0。输入数组也计入 RSS，不能把 RSS 当作树自身大小。

| 树 | 规模/场景 | 0 | 2 | 4 | 8 |
|---|---|---:|---:|---:|---:|
| ART | 100k/hot | 203.875 / 6.895 / 2075.195 | 84.083 / 6.895 / 628.406 | 62.484 / 6.950 / 376.977 | 48.906 / 7.040 / 212.355 |
| ART | 100k/long | 249.156 / 6.895 / 10375.977 | 147.568 / 6.895 / 3320.105 | 120.844 / 8.416 / 1926.991 | 115.755 / 13.519 / 991.336 |
| ART | 100k/lookup90 | 190.714 / 6.895 / 206.113 | 31.500 / 6.895 / 5.757 | 31.344 / 6.947 / 0.096 | 31.312 / 7.033 / 0.000 |
| ART | 100k/uniform | 203.750 / 6.895 / 2075.195 | 89.948 / 6.895 / 553.621 | 71.641 / 8.502 / 247.388 | 49.854 / 14.971 / 28.812 |
| ART | 1m/uniform | 341.797 / 68.937 / 4272.461 | 164.208 / 68.937 / 346.116 | 133.516 / 71.863 / 15.958 | 135.885 / 76.910 / 0.017 |
| AERT | 100k/hot | 72.380 / 7.325 / 427.246 | 52.349 / 7.325 / 129.378 | 39.406 / 7.379 / 80.339 | 36.422 / 7.470 / 47.656 |
| AERT | 100k/long | 120.922 / 7.325 / 2136.230 | 92.505 / 7.325 / 683.551 | 89.141 / 8.845 / 410.670 | 93.245 / 13.949 / 222.472 |
| AERT | 100k/lookup90 | 32.786 / 7.325 / 42.426 | 32.172 / 7.325 / 1.185 | 31.969 / 7.376 / 0.021 | 31.969 / 7.462 / 0.000 |
| AERT | 100k/uniform | 72.141 / 7.325 / 427.246 | 46.224 / 7.325 / 113.981 | 43.359 / 8.932 / 52.722 | 43.312 / 15.400 / 6.466 |
| AERT | 1m/uniform | 185.745 / 73.242 / 976.479 | 146.708 / 73.242 / 79.104 | 139.458 / 76.168 / 3.756 | 142.406 / 81.215 / 0.004 |

2 槽没有新增 reachable bytes；1m Uniform 相比 slot 0，ART 峰值 RSS 降约 52%，
AERT 降约 21%。4/8 槽在部分场景峰值更低，但增加最终树内存。
这支持内存优先显式选择 2 槽，不代表 2 槽在所有内存指标/场景下都最优。
CLI 默认暂不修改，Zipfian 后续补测见文末，大规模多轮及 sanitizer 验收仍独立跟踪。
slot 0 仍使用原有 SeqCow 提交协议，本报告不将它视为已证明并发语义等价的基线。

## 复现命令

```sh
make -B test-fat-radix test-fat-btree test-fat-betree bench-fat-radix CC=clang++
python3 bench/generate_radix_workloads.py build/mac-validation/100k --records 100000
python3 bench/generate_radix_workloads.py build/mac-validation/1m \
  --records 1000000 --operations 1000000 --scenes uniform
python3 bench/fat_slots_bench.py ./bench/art_fat_bench \
  build/mac-validation/100k/radix_uniform.data \
  build/mac-validation/100k/radix_hot.data \
  build/mac-validation/100k/radix_lookup90.data \
  build/mac-validation/100k/radix_long.data \
  --runs 3 --warmups 1 --clients 1 --gc --output bench/results/art_fat_mimalloc_100k_arm64.csv
```

AERT 替换驱动和 CSV 名称；1m 仅传入对应 uniform 文件。
设置 `MIMALLOC_PREFIX=/path/to/prefix` 可覆盖自动检测。完整 main 仍有原项目 x86
编译选项，Mac 原生实验使用上述独立 bench 目标。

## Sanitizer 环境诊断

Xcode clang 17 (clang-1700.6.4.2) 与 Command Line Tools clang 17
(clang-1700.0.13.5) 的简单打印程序均为：UBSan 通过，ASan 在 6–8 秒超时前
未进入 main，TSan SIGSEGV。sandbox 内外结果相同，均不依赖 mimalloc。
因此不能通过缩小数据规模解决，也不能把当前 ASan/TSan 结果计为通过。
上游有[类似的 macOS ASan 启动挂起报告](https://github.com/llvm/llvm-project/issues/200447)，
但尚未用堆栈证明本机故障与该报告具有同一原因。

尝试通过现有 Homebrew 安装新版 LLVM：
`HOMEBREW_NO_AUTO_UPDATE=1 ~/.brew/bin/brew install llvm`。
安装未成功：下载 zstd bottle manifest 时访问 `ghcr.io:443` 出现
`curl (35) LibreSSL SSL_connect: SSL_ERROR_SYSCALL`。另因现有非标准 Homebrew
前缀，该 LLVM 安装计划会从源码构建。未把下载失败描述为 sanitizer 已修复，
也未为了安装工具链修改现有 Homebrew tap 信任设置或迁移前缀。
当前可用范围是 mimalloc 原生构建、功能/并发测试、UBSan 和本报告内存实验。

## 再次尝试与 Zipfian 补测

按用户“再试一下，不行先不管”的要求进行了最后一轮环境重试。
Xcode ASan 仍在 8 秒内未进入 main，TSan SIGSEGV；已安装 LLVM 15 的 ASan
在初始化阶段触发 `sanitizer_malloc_mac.inc:191` CHECK，TSan SIGSEGV。
探针在 sandbox 外运行，不涉及树或 mimalloc；原始诊断见
[mac_sanitizer_retry.json](mac_sanitizer_retry.json)。
新版 LLVM 23.1.0 的下载重试成功，但 Homebrew 随后因 Command Line Tools
过旧拒绝安装，提示需要 Xcode 26.3 对应的 Command Line Tools。
没有删除、替换或升级系统工具链。ASan/TSan 暂时搁置，仍标记为未通过验证。

Zipfian 已补测：有限分布 `P(rank=k) = k^-0.99 / sum(j^-0.99)`，逆 CDF 采样，
固定随机种子 182；用独立种子 183 将 rank 随机排列到 key。
这是真正的有限 Zipf 分布，并非此前的 hot 模型；也不宣称与 YCSB-C 的
scrambled Zipfian 实现逐项一致。默认原四场景不变，显式 `--scenes zipfian` 启用。
输入 SHA256、概率及范围验证见
[zipfian_workload_metadata.json](zipfian_workload_metadata.json)。

100k 初始记录、500k 更新；1m 初始记录、1m 更新。其余参数与前述 mimalloc
矩阵一致：每配置 1 次 warmup、3 次测量、0/2/4/8 槽、单 client、GC on。
新增 48 条测量均 GC pending nodes/bytes 为 0，retired/reclaimed 字节相等。
总计已完成 168 条真实 mimalloc 测量记录。

峰值 RSS / reachable requested / 累计 retired，均值 MiB：

| 树 | Zipfian 规模 | 0 | 2 | 4 | 8 |
|---|---|---:|---:|---:|---:|
| ART | 100k | 203.521 / 6.895 / 2075.195 | 85.740 / 6.895 / 592.261 | 63.016 / 7.402 / 328.486 | 49.750 / 8.744 / 169.333 |
| ART | 1m | 341.797 / 68.937 / 4272.461 | 178.234 / 68.937 / 1067.632 | 156.896 / 69.821 / 592.092 | 145.594 / 72.099 / 306.776 |
| AERT | 100k | 72.141 / 7.325 / 427.246 | 45.578 / 7.325 / 121.936 | 40.568 / 7.831 / 70.005 | 38.151 / 9.173 / 38.001 |
| AERT | 1m | 186.682 / 73.242 / 976.499 | 148.484 / 73.242 / 244.016 | 143.682 / 74.127 / 139.370 | 142.771 / 76.405 / 75.486 |

复现：

```sh
python3 bench/generate_radix_workloads.py build/mac-validation/100k \
  --records 100000 --scenes zipfian
python3 bench/generate_radix_workloads.py build/mac-validation/1m \
  --records 1000000 --operations 1000000 --scenes zipfian
python3 bench/fat_slots_bench.py ./bench/art_fat_bench \
  build/mac-validation/100k/radix_zipfian.data \
  build/mac-validation/1m/radix_zipfian.data \
  --runs 3 --warmups 1 --clients 1 --gc \
  --output bench/results/art_fat_mimalloc_zipfian_arm64.csv
```

AERT 替换驱动和输出文件名。Mac 上计划的本地规模场景补测已完成；
ASan/TSan 尚未通过且经用户同意暂时搁置，不将其解释为完整 sanitizer 验收。
论文规模跨平台复现与 ConCow 实现仍属于后续工作。
