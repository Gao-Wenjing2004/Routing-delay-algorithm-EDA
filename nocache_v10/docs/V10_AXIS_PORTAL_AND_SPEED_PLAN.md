# V10：71/72、单 Block 结构替换与十亿请求提速门禁

## 当前基线与边界

可信 1B 官方结果为 `total=95.30, accuracy=97.56, time=81.69, consistency=100`。
本轮不能把 P10 直接用于距离 71/72：P10 表示跨完整 100 行周期的 Portal 中段；
71/72 的无障碍段应由 AXS9 全距离轴向转移处理，跨周期中段才使用 P10。

所有实现禁止保存或命中 `From,To -> Delay` 请求结果。允许的资产仅是由固定 FPGA
架构导出的不可变表，例如端口局部连接器、方向状态转移、Gap 前缀、Block Portal 与 P10。

## 已实现的研究门禁

1. `export_compact_graph.py` 可同时导出端口局部模型和精确 Arc+Net 状态转移。
2. `select_axis_portal_probe.py` 从 train、完整源隔离、完整区域隔离三面板分别选取
   距离 71、72、Block 穿越与 Block 邻域样本，并同步过滤 Golden 摘要和 K48 trace。
3. `analyze_axis_portal_oracle.py` 组合源局部连接、H/V 状态转移、Portal 候选、目标局部连接，
   对实际候选线段逐段加入 Gap/Block 费用。
4. `gate_axis_portal_replacement.py` 要求三面板覆盖率至少 99%、候选从不低于 Golden、
   替换 Accuracy 全部为正。该门禁即使通过，也只允许实现 C++ 固定查询，不直接允许删表。
5. 只有 C++ 候选再次通过逐字节输出、三面板 Accuracy、1B 时间投影和 100 MB 体积门禁后，
   才允许删除 Short48、Hull 和被覆盖的残差资产。

当前 Block 语义存在一个必须由门禁捕获的细节：可横穿 Block 时，缺失 SRB 不消耗 Net
位移计数。因此“基础轴向延时 + Block 固定罚时”不一定等于真实空间转移。若候选出现
`Predicted < Golden`，不得调阈值掩盖，必须改用 P10 或预计算的精确局部 Portal 转移。

## 时间优化候选

新增 `estimate_soa_mt.cpp`，不改变任何预测值，只改变执行流水：

- 16 MB 大块读入；
- 一次扫描把两个端点解析成整数 SoA；
- 每 32 条为一小块，使用 1/2/4/8 线程只读共享模型并行预测；
- 单线程按原请求顺序格式化和写出；
- 分别报告资产加载、读取、解析、预测、格式化和写出时间；
- 每个线程数都必须与 K48 输出 `cmp` 逐字节一致。

这不是请求缓存：每条请求仍完整解析、完整预测，重复与否不会改变算法分支或查询次数。

## Linux 优化依据与下一步

- Linux `madvise(2)` 明确区分随机页访问 `MADV_RANDOM` 与顺序访问
  `MADV_SEQUENTIAL`。AXS9/P10 的随机只读表适合测试前者，输入 CSV 的流式读入不应套用同一策略：
  <https://man7.org/linux/man-pages/man2/madvise.2.html>
- `MAP_POPULATE` 会预建页表/预读，可能把缺页开销提前到启动阶段，因此不默认启用；必须比较
  冷启动总时间而不是只比较稳态循环：<https://man7.org/linux/man-pages/man2/mmap.2.html>
- GCC `target_clones` 可以产生 default/AVX2 多版本并在运行时分派；正式包不能只用
  `-march=x86-64-v3` 假设官方 CPU：<https://gcc.gnu.org/onlinedocs/gcc/Common-Attributes.html>
- GCC 的 `-fprofile-use` 要求训练 profile 与源码、优化选项匹配。本项目应使用唯一请求、
  多分支覆盖的训练流，而不是公开重复数据：<https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html>
- `perf stat -d` 可报告 L1/LLC，继续增加 `-d` 可看 TLB；只有确认预测阶段是 Cache/TLB
  受限，才值得继续位压缩和直接映射：<https://www.man7.org/linux/man-pages/man1/perf-stat.1.html>

实施顺序固定为：先得到 SoA/线程的逐字节与阶段计时结果；然后把通过准确度门禁的 AXS9/P10
资产改为只读直接映射，并比较 `MADV_NORMAL`、`MADV_RANDOM`、可选预热；最后才在代表性唯一
请求上做 PGO/LTO 与 default/AVX2 运行时分派。任何只提高重复公开集、却不能提高完整源和完整
区域隔离集的模块都不得进入提交包。
