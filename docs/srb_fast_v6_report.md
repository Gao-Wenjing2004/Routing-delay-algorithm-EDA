# SRB Fast V6 探索、实现与验证报告

日期：2026-08-28  
状态：V6 实验版本已实现；公开 100 万条全量验证完成；隐藏 1 亿条官方真实运行尚不可获得。

## 1. 结论先行

V6 没有照搬示意图中的所有分支，而是只接入了已经在公开数据和多种固定留出集上表现稳定的部分：

- 保留 V3/V4 的规范解析、O(1) 基础预测、Gap 前缀和、Block 上下文和 Atlas。
- 将 Atlas 查询半径由 48 扩大到 56，公开分支覆盖由约 9.0182% 提高到 10.9273%。
- 对 Atlas 之外的请求加入 Macro10、216 个端口 family、端点绝对相位的紧凑残差。
- 用坐标查表替代热点路径中的取模；优化前后百万条输出哈希一致。
- 未接入 Exact/Template Cache、Source/Target Tree、结构模型选择器和 Portal Graph。前几项在公开查询上复用率过低；后两项尚未通过外层留出或缺少严格路径影响标签。

最终公开 100 万条 `acc_score=95.637129`、MAE=28.183614 ps。以同机 10 轮均值线性投影 1 亿条为 67.521067 秒，按当前评分合同估算总分 95.947028。它比同批 V3 的估算总分高 0.209211，比 compact V4 高 0.184980。以上不是隐藏集官方成绩。

## 2. 依据和评分口径

阅读和复用的主要资料：

- `2026创“芯”·EDA精英挑战赛赛题Q&A（紫光同创）.docx`
- `最新修改版-赛题一：面向超大规模FPGA的布局布线延时估算算法（紫光同创）(1)(1)(2).pdf`
- `plusone-srb_fast_v3/srb_fast_v3`、`srb_fast_v4`、`srb_fast_v5`
- `analysis/public_queries_full`、V4/V5 实验产物和正式评分工具

本地严格评分合同：

```text
point_i = 1 - tanh(4 * abs(pred_i - golden_i) / golden_i)
acc_score = 100 * mean(point_i)
time_score = 100 * (1 - average_seconds / 1800)
total = 0.80 * acc_score + 0.15 * time_score + 0.05 * consistency_score
```

正式材料要求运行 5 次；输出完全一致时 `consistency_score=100`。官方资料未完整定义零 Golden、提交包体积、离线 Atlas/公开 Golden 权重的可接受边界，详见 `docs/v4_score_contract.md`。因此本文只把总分写作“估算总分”。

## 3. 图片方案的逐项处理

| 图片中的策略 | 数据证据 | V6 决策 | 结论类别 |
|---|---:|---|---|
| Query Canonicalization | 正式解析器已有一致端点规则 | 保留 | 已实现 |
| Exact Query Cache | 1,000,000 个 exact key，首见后命中 0% | 不接入热点路径 | 安全但公开无收益 |
| Source Tree Top-1000 | 覆盖 0.3268% | 不预建 | 理论机会过低 |
| Target Reverse Tree Top-1000 | 覆盖 0.3090% | 不预建 | 理论机会过低 |
| 几何 Template Cache | 原始模板复用上界约 0.056%；保守环境签名后安全命中为 0 | 不接入 | 几何相同不等于 delay 相同 |
| 短距离 Atlas | V3 半径 48 约 9.0182%；半径 56 为 10.9273% | 扩到 56 | 已实现，仍是估算 Atlas |
| Macro/周期 | 多周期与多留出消融支持 period=10 的残差特征 | 接入 Macro10 残差 | 经验结构，不是精确模板 |
| Block Portal | Block bbox 47.7308%，但 bbox 不是实际路径受阻标签 | 不接入 | 尚未验证 |
| ML/深度学习兜底 | 单 residual 可常数时间执行；深网会增加推理风险 | 使用紧凑表残差，不上深网 | 已实现 |

公开数据无 Block bbox 的请求占 52.2692%，说明普通区域周期建模值得研究；它不证明任意位移可安全平移复用。Block bbox 与最短路径是否实际受 Block 影响也不能等同。

## 4. V6 架构

### 4.1 分支

```text
端点解析与规范化
  ├─ 同端点：既有规则
  ├─ 无 Block、Atlas 可索引、Chebyshev <= 56：legacy Atlas
  └─ 其余：V3 O(1) 基础预测 + Macro10/family/phase residual
```

V6 不在查询时运行 A* 或 Dijkstra，也不构建每请求复杂对象。完整请求处理仍为常数次数的整数解析、表索引与浮点加法。

### 4.2 残差

最终候选为 `port_family_macro10_family_phase`：

- 216 个端口 family；规则是去除末尾十进制索引，其余端口名称完整保留。
- 公共预测、距离、边界、Gap、Block 距离表。
- family-pair、source/target family-distance 表。
- `(dx mod 10, dy mod 10)` 与方向表。
- source/target family × Macro10 表。
- source/target 绝对坐标 phase，以及 family × phase 表。
- 205,932 个 float32 参数，823,728 字节。

模型在固定 `random_stratified` 训练侧拟合：800,304 个训练行，199,696 个固定留出行从未用于效果拟合；实际 residual 训练行 728,040。固定留出相对重拟基线提升 +0.126410 `acc_score`。生产模型仍混合公开训练/留出上的最终预测结果，隐藏集泛化不能由此证明。

## 5. 周期调查

所有数值为“在各划分训练侧重新拟合、在验证侧计算”的 `acc_score` 增量；并非在全量 Golden 上挑最好单元格。

### 5.1 4/8/12/16 初筛

| 周期 | 九个固定划分平均增益 | 最小增益 | 参数量 |
|---:|---:|---:|---:|
| 4 | +0.124593 | +0.084774 | 125,488 |
| 8 | +0.112341 | +0.053731 | 146,656 |
| 12 | +0.147046 | +0.104582 | 181,936 |
| 16 | +0.104350 | +0.055497 | 231,328 |

这否定了“Net 最大跨度为 8，所以周期必然为 8”的直接推断；12 在这组候选中更好，但仍需局部搜索。

### 5.2 局部细化

| 周期 | 九个固定划分平均增益 | 最小增益 | 参数量 |
|---:|---:|---:|---:|
| 6 | +0.131486 | +0.086153 | 134,308 |
| 9 | +0.086938 | +0.059894 | 154,153 |
| 10 | **+0.153458** | **+0.108429** | 162,532 |
| 11 | +0.084120 | +0.057477 | 171,793 |
| 12 | +0.147046 | +0.104582 | 181,936 |
| 14 | +0.111307 | +0.078447 | 204,868 |

10 明显优于相邻 9/11，也略优于 12，所以选 Macro10。它可能综合吸收了架构、Gap、Block 和边界尺度，但当前数据不足以把原因解释为严格物理周期。

### 5.3 相位特征

| Macro10 变体 | 九划分平均增益 | 最小增益 | 参数量 |
|---|---:|---:|---:|
| endpoint | +0.153458 | +0.108429 | 162,532 |
| + absolute phase | +0.155277 | +0.114730 | 162,732 |
| + family absolute phase | **+0.159959** | **+0.119769** | 205,932 |

另用完全未见的 Macro10 remainder/absolute-phase 单元做专门留出，最终变体最小仍为 +0.081006，说明收益并非只来自记住同一余数格；但这仍是公开架构上的经验泛化证据。

## 6. Atlas 半径消融

残差权重固定，只有运行时 Atlas 查询半径变化：

| 查询半径 | 公开 `acc_score` | MAE/ps |
|---:|---:|---:|
| 0 | 94.252707 | 30.921933 |
| 32 | 95.140930 | 29.485943 |
| 40 | 95.335225 | 29.031146 |
| 44 | 95.419335 | 28.816979 |
| 48 | 95.496539 | 28.600043 |
| 52 | 95.574885 | 28.374955 |
| 56 | **95.637129** | **28.183614** |

56 相对 48 在 11 个公开诊断切片中没有负增益；49–56 距离段相对 compact V4 提升 +4.579037。半径 56 的 Atlas 条目源自现有资产，但某些超出原 query radius 的 seed residual 会被忽略，因此不能标记为严格精确结果。

将 residual 自身的训练排除半径也改为 56，公开分数反而低 0.000336，所以最终权重仍按半径 48 的训练 mask 生成，运行时 Atlas 使用 56。

## 7. 分段结果

| 分段 | 行数 | V3 acc | V4 acc | V6 acc | V6-V4 |
|---|---:|---:|---:|---:|---:|
| 全部 | 1,000,000 | 95.308549 | 95.353385 | 95.637129 | +0.283744 |
| Atlas radius 56 | 109,274 | 95.136705 | — | 96.478683 | — |
| radius 56 fallback | 890,726 | 95.329631 | 95.378132 | 95.533887 | +0.155755 |
| Block bbox | 477,308 | 95.875284 | 95.924456 | 96.085588 | +0.161131 |
| 无 Block bbox | 522,692 | 94.791023 | 94.831899 | 95.227608 | +0.395710 |
| distance 49–56 | 32,328 | 90.653726 | 90.726167 | 95.305204 | +4.579037 |
| distance 57–128 | 278,051 | 93.559931 | 93.616177 | 93.812725 | +0.196547 |
| distance 129–256 | 306,892 | 95.821305 | 95.870127 | 96.027175 | +0.157048 |
| distance 257+ | 272,711 | 97.296028 | 97.336136 | 97.440649 | +0.104513 |

公共生产模型在 random、spatial、long、block、gap、rare-port、boundary、Macro8/10 remainder/phase 等切片上均高于 V4。更严格的证据来自每个 split 都只在训练侧重拟的消融，而不是这张全量生产模型切片表。

## 8. 被否决的结构选择器和 Portal

V5 中不依赖 Golden 请求行的结构预测虽然很快，但直接替代残差时公开 `acc_score=92.373770`，低于测试基线 95.398901。用训练侧学习“何时切换结构预测”的外层留出选择器也不稳定：

- geometry 选择器七划分平均 -0.003103，最差 -0.013082。
- family 选择器平均 -0.001687，最差 -0.012660。
- family-pair 选择器平均 -0.001508，最差 -0.011523。

因此 V6 没有为了提高结构分支占比而接入它。Portal Graph 同样未进入 V6：现有 Block bbox 只能说明端点包围盒相交，不能说明最短路确实受阻，也没有独立 exact solver 标签证明 Portal 压缩保持最短延时。

## 9. 正式性能与估算总分

同一个 `estimate_v6.exe`，同一输入/Atlas，同机交错运行，每模式 10 轮：

| 模式 | acc_score | query+I/O 均值/s | 中位数/s | 1 亿线性投影/s | 峰值 RSS/MiB | 输出一致 | 估算总分 |
|---|---:|---:|---:|---:|---:|---|---:|
| V3 | 95.308549 | 0.596444 | 0.584759 | 61.082756 | 645.992 | 是 | 95.737817 |
| compact V4 | 95.353385 | 0.610428 | 0.607158 | 62.479325 | 646.001 | 是 | 95.762047 |
| V6 | **95.637129** | 0.660883 | 0.655280 | 67.521067 | 646.671 | 是 | **95.947028** |

Atlas 一次加载约 1.4 秒已经包含在投影公式中。百万条的 V6 输出 SHA256 为 `2CDF8A66373ECA8E37CCAC87086A8E7843D71E81A42742B470AEF8BA000165DC`。

另做 500 万条重复输入 5 轮压力测试，输出哈希一致。系统负载导致前两轮偏慢：query+I/O 均值 5.006313 秒，中位数 4.702144 秒；其线性投影不作为最终同机排名依据，保留为吞吐稳定性证据。

## 10. 正确性和回归校验

- V6 百万条 Python/C++ 输出逐行一致。
- 取模查表优化前后 1,000,000 行不一致数为 0。
- V3 回归 SHA256：`D39ED2678619CCF791730A672EB7A7F0CB3226EB55FFB5E605A9030043996F75`。
- 正式 compact V4 回归 SHA256：`BDC0F47A63EF55D0A71717779864EA9E05B8AAAA371E9A471252D794C6A2AAEC`。
- V3/V4 默认 Atlas 半径保持 48；V6 默认 56，实验开关不改变正式 V4 默认行为。
- 三模式 10 轮各自输出哈希一致。
- V6 资产校验包含架构 SHA256、Atlas SHA256/CRC32、模型头文件 SHA256 和参数量。

## 11. 运行命令

构建和正式运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\srb_fast_v6\build_v6.ps1

.\srb_fast_v6\estimate_v6.exe `
  -in .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  -out .\analysis\srb_fast_v6\v6_public_1m.csv `
  -atlas .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
  --mode v6
```

同机 10 轮基准：

```powershell
C:\Users\yanggoo\anaconda3\python.exe .\tools\benchmark_srb_v4.py `
  --executable .\srb_fast_v6\estimate_v6.exe `
  --input .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  --atlas .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
  --output-dir .\analysis\srb_fast_v6\benchmark_final_optimized_1m `
  --sizes 1000000 --runs 10 --modes v3,v4-optimized,v6
```

## 12. 仍不能确定的事项

1. 隐藏请求是否保持公开数据的端口、距离、Block/Gap 和相位分布。
2. period=10 是否具有物理结构解释；当前只证明它是更好的经验残差特征。
3. Atlas 半径 56 对新架构/边界是否仍稳定；当前资产和指纹锁定同一架构。
4. Block bbox 内哪些请求的真实最短路径受 Block 影响，以及 Portal 压缩是否精确。
5. 官方环境上的真实 1 亿条时间、峰值内存限制、离线 Atlas 与 Golden 训练权重是否被接受。

## 13. 下一步最值得做的实验

1. 用正式 exact solver 在分层抽样的小规模长路径集上生成路径标签，检验 Macro10 残差和物理周期是否一致。
2. 对 Block bbox 请求区分“bbox 相交 / 候选直达 Net 相交 / exact 路径实际受阻”，再做 Portal 最小原型。
3. 从 Atlas seed residual 的有效范围入手，构造半径 56 的严格可达性验证，判断能否把当前估算 Atlas 升级为精确分支。
4. 用多个合成查询分布做 shift 压测，尤其提高罕见 family、边界和无 Gap 请求比例。
5. 若官方允许更大离线表，再比较半径扩展的准确收益、623.5 MiB 固定内存和提交包限制；否则研究压缩 Atlas。

## 14. 主要产物

- `srb_fast_v6/estimate_v6.cpp`、`estimate_v6.exe`
- `srb_fast_v6/v6_macro10_data.hpp`
- `srb_fast_v6/v6_model_manifest.json`
- `srb_fast_v6/verify_v6_assets.py`
- `srb_fast_v6/build_v6.ps1`、`build_v6.sh`
- `tools/analyze_srb_v6.py`
- `analysis/srb_fast_v6/period_*`
- `analysis/srb_fast_v6/macro10_*`
- `analysis/srb_fast_v6/atlas_radius_ablation`
- `analysis/srb_fast_v6/segment_analysis`
- `analysis/srb_fast_v6/benchmark_final_optimized_1m`
- `analysis/srb_fast_v6/benchmark_lookup_optimized_5m`
