# V9：精确数据记忆 + Portal/Landmark 有界搜索 + V8 回退

V9 是满足 100 MB 限制的单文件提交程序。当前提交版包含三条路径：

1. `Chebyshev <= 8` 时，用离线精确 Dijkstra 数据蒸馏出的 3 张小表修正 V8；
2. 极少数固定结构请求进入最大 16 次展开的精确 A*；
3. 未被覆盖或搜索预算耗尽的请求立即使用 V8，不输出未完成的候选路径。

提交版不携带 65 MB 外部全图，也不执行每查询一次的反向 Dijkstra。Arc+Net 已收缩为宏边，Block/Gap
语义在紧凑图中按规则生成；26 个 Grid/Block-Portal Landmark 为 A* 提供有向可采纳下界。

## 当前结果

- 公开 1,000,000 条：V8 `94.603473961`，当前 V9 `94.613722680`；
- 默认精确分支：选择 10 条、完成 10 条、总展开 34 个状态；
- `Chebyshev <= 16` 全选审计：cap=16 完成 225 条，225/225 与 Golden 一致；
- Windows 等价程序连续三次百万条输出 SHA-256 完全相同；
- Linux 静态提交文件：20,733,408 字节，小于 100,000,000 字节。

完整实验、哪些设想已经实现以及尚未完成的 Portal overlay，见
[V9_P3_EXACT_DATA_AND_PORTAL_RESULTS.md](docs/V9_P3_EXACT_DATA_AND_PORTAL_RESULTS.md)。

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
  --count 1000000 --source-count 200 --max-cheb 64 \
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
