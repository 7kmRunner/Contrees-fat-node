# SeqCow 基线：开始 ConCow 实验前

本地可回退快照：`build/baselines/seqcow-before-concow.tar.gz`。
文件清单及各文件 SHA256：`build/baselines/seqcow-before-concow.manifest.json`。
归档 SHA256：`c4cfa57148e6ec5a0cd90bad1028b68a2f18035a6badef3b9fa63f6ca7fd5451`。
归档保留当时工作区的源文件、实验脚本、文档和结果，不包含二进制、输入数据或 Git 元数据。
验证归档内容后应解压到另一个目录比对，不直接覆盖有新修改的工作区。
本次未将工作区原有大量未提交修改一并提交到 Git。

## 已验证范围

- SeqCow：B+Tree、BeTree、ART、AERT fat leaf，槽数 0/2/4/8。
- ART/AERT 叶节点维持 64 bytes；2 槽内联，4/8 槽按需 overflow。
- 四棵树的功能回归；ART/AERT 并发快照、GC、mimalloc 下 UBSan。
- Mac mimalloc 3.5.0 实验 168 条记录，包括 100k/1m Uniform 和有限 Zipfian。
- 结果入口：`bench/results/mac_mimalloc_validation.md`。

## 保留限制

- ASan/TSan 在本机运行时失败；用户同意暂时搁置，不能标记为通过。
- 原 slot-0 调度路径的并发提交语义尚非经过证明的等价基线。
- 未完成论文规模跨平台实验；ART/AERT CLI 默认仍为 0，手动选择 2 可减少额外常驻内存。
- ConCow fat-node 第一阶段通过独立实验驱动开展，未开放主程序的 `concow --fat-slots`。
