# V9：精确数据记忆 + Portal/Landmark 有界搜索 + V8 回退

V9 是满足 100 MB 限制的单文件提交程序。当前 P8 提交版包含五条路径：

1. `Chebyshev <= 8` 时，用离线精确 Dijkstra 数据蒸馏出的 3 张小表修正 V8；
2. 极少数固定结构请求进入最大 16 次展开的精确 A*；
3. P7 Teacher 位图命中的短请求使用静态 Beam5 结构路径和 160 状态表定价；
4. 无 Block、距离 17～64 且通过独立 Teacher 消融的请求使用静态宏段 Beam2 定价；
5. 未被覆盖、结构候选不可达或搜索预算耗尽的请求立即使用 V8/P3，不输出未完成的候选路径。

提交版不携带 65 MB 外部全图，也不执行每查询一次的反向 Dijkstra。Arc+Net 已收缩为宏边，Block/Gap
语义在紧凑图中按规则生成；26 个 Grid/Block-Portal Landmark 为 A* 提供有向可采纳下界。

## 当前结果

- 公开 1,000,000 条：P3 `94.613722680`，P7 `94.740555805`，当前 P8 `94.825754730`；
- 默认精确分支：选择 10 条、完成 10 条、总展开 34 个状态；
- P7 结构分支：选择 7,860 条、完成 6,833 条；
- P8 宏段分支：选择 7,540 条、完成 6,120 条；
- `Chebyshev <= 16` 全选审计：cap=16 完成 225 条，225/225 与 Golden 一致；
- Windows 等价程序连续五次百万条输出 SHA-256 完全相同；
- Linux 静态提交文件：43,826,696 字节，小于 100,000,000 字节。

完整实验、哪些设想已经实现以及尚未完成的 Portal overlay，见
[V9_P3_EXACT_DATA_AND_PORTAL_RESULTS.md](docs/V9_P3_EXACT_DATA_AND_PORTAL_RESULTS.md)。

附件路线收紧后的短距离 PRP-R P0/P1 实验见
[V9_P4_PRPR_SHORT_PATH_FINDINGS.md](docs/V9_P4_PRPR_SHORT_PATH_FINDINGS.md)。P4 已证明“正确骨架 +
160 状态周期原语 + 逐段 Gap 计费”可以精确复现审计 Golden，但当前骨架生成和部分目标端口连接尚未达到
Go/No-Go 指标，因此正式提交二进制仍保持 P3。

目标端连接修复、17 类骨架、弹性 Dijkstra 模板、Beam 32 与逐段 Gap 的 P5 结果见
[V9_P5_STRUCTURED_GENERATOR_RESULTS.md](docs/V9_P5_STRUCTURED_GENERATOR_RESULTS.md)。P5 已把端到端结构链路打通，
但结构候选数尚未通过 C++ 微秒成本门槛，因此也没有替换正式提交二进制。

支持度统一评分、全局 Beam32、单段变体、C++ 表驱动定价和 1,838 条留出消融见
[V9_P6_GLOBAL_BEAM_PRICER_RESULTS.md](docs/V9_P6_GLOBAL_BEAM_PRICER_RESULTS.md)。P6 又把生成器压成 4.28 MB
六层表和 Beam5：完整公开 1M Accuracy 为 `94.740555805`。P7 已将最终 Beam5、轴向状态转移和 Top-32
边全部静态嵌入，并复用 P3 已解析字段；正式结果见
[V9_P7_STATIC_EMBEDDED_RESULTS.md](docs/V9_P7_STATIC_EMBEDDED_RESULTS.md)。中距离定向 Teacher、宏段表、
Beam2 与少量 A* 的 P8 结果见
[V9_P8_MACRO_TEACHER_RESULTS.md](docs/V9_P8_MACRO_TEACHER_RESULTS.md)。当前提交版已切换到 P8。

精确图证明、周期 Portal 闭包及局部连接器研究见
[V9_P9_EXACT_GRAPH_PERIODIC_PORTAL_RESULTS.md](docs/V9_P9_EXACT_GRAPH_PERIODIC_PORTAL_RESULTS.md)、
[V9_P10_DIRECTIONAL_PERIODIC_PORTAL_MATRIX.md](docs/V9_P10_DIRECTIONAL_PERIODIC_PORTAL_MATRIX.md)、
[V9_P11_LOCAL_PORTAL_CONNECTOR_RESULTS.md](docs/V9_P11_LOCAL_PORTAL_CONNECTOR_RESULTS.md) 和
[V9_P12_CERTIFIED_PORTAL_PRUNING_AND_PAPER_FINDINGS.md](docs/V9_P12_CERTIFIED_PORTAL_PRUNING_AND_PAPER_FINDINGS.md)。
P12 已在定向 Block Teacher 上用有证书的分支限界保持 `99.087925%` 研究上限，并用条件出口表把组合数
压到约 1,528 对时取得 `98.451288%`；但局部连接器尚未批量化，正式提交仍保持 P8。

## 构建与运行

Linux：

```bash
cd srb_hybrid_v9
./build.sh
./submission/bin/estimate -in request.csv -out result.csv -threads 1
```

Windows 上用 Zig 交叉编译 Linux 静态文件：

```powershell
.\build_linux_with_zig.ps1 -ZigExe C:\path\to\zig.exe
```

## 生成精确训练数据

先生成按完整源端点分组的合法请求：

```bash
python3 tools/generate_extra_queries.py \
  --inst arch/SRB_Inst.json \
  --port arch/SRB_Port.json \
  --template-requests public_requests.csv \
  --count 1000000 --source-count 226 --minimum-sources-per-port 2 --max-cheb 64 \
  --output exact_requests.csv --manifest exact_manifest.json
```

再用共享源的一对多 Dijkstra 标注：

```bash
./offline_grouped_labeler \
  --graph srb_graph.bin --input exact_requests.csv \
  --labels exact_labels.csv --statistics exact_stats.csv \
  --max-group-expanded 10000000
```

需要路径本身与 Portal 摘要时，使用 `offline_exact_labeler.cpp`；大规模训练只需要延时标签时，优先使用
`offline_grouped_labeler.cpp`。训练/验证必须按完整源端点隔离，不能把相同源端点随机拆到两侧。
