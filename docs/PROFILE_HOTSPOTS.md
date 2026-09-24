# 已有 perf 样本的热点指令定位

这一步不运行基准、不生成数据、不重新采样，也不需要提升 perf 权限。
读取成功采样目录中的 metadata.json、profiles/r*-stacks.txt、profiles/r*.data 和 bin/original、bin/fat。

```bash
python3 -u bench/profile_hotspots.py build/paper-profile-20260923-155323-2515593
```

依赖 Python 3.9+ 和 GNU binutils 的 objdump、addr2line，Ubuntu build-essential 通常已带入 binutils。
脚本按进程和 MMAP/MMAP2 映射把样本 IP 转成文件偏移，验证二进制 SHA256，
再通过 ELF PT_LOAD 转成反汇编地址。避免直接将 ASLR 地址当作固定指令地址。
每组按客户端、worker、monitor、流水线、fat 探测、COW、其他类别各选前三个热点地址。
用 objdump 生成指令上下文，并仅对有限地址调用一次 addr2line；源码行解析失败不丢弃指令报告。
默认每个二进制 objdump 上限 60 秒、addr2line 上限 30 秒，不做全量逐样本源码行解析。

输出 build/profile-hotspots-时间/ 和同级 .tar.gz：
- hotspots.csv：样本计数、总样本百分比、文件偏移、函数。
- selected.json：选定热点及 ELF 虚拟地址。
- original/fat-instructions.txt：热点附近的反汇编。
- original/fat-source-lines.txt：有限地址的源代码位置（可能含内联链）。
- metadata.json：来源、样本数、地址映射遗漏和工具执行状态。

回传 RESULT_ARCHIVE 对应文件即可。指令报告用于区分自旋读、条件跳转、原子操作等，
不能仅凭样本占比推导墙钟瓶颈或断言因果；仍需结合具体更新协议和受控实验。

本机验证：九份已有调用栈解析出 107330 个样本，与原始 perf SAMPLE 记录数逐文件一致；
目标二进制内样本映射没有遗漏。ELF 文件偏移/虚拟地址不相等的转换边界测试通过。
服务器实际 objdump/addr2line 执行尚需确认。
