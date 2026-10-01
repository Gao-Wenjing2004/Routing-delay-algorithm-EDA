# SRB Fast V4：Q&A 约束、长路径复用与自动探索报告

日期：2026-08-28。

本报告只评价 `srb_fast_v4` 及其独立实验分支。`srb_fast_v5` 仅用于理解“不依赖 Golden 的运行时结构预计算”这一设计思路，其成绩不计入 V4 表格，也不作为 V4 结论。

## 1. Q&A 中已确认的规则

来源：`2026创“芯”·EDA精英挑战赛赛题Q&A（紫光同创）.docx`。

- Q&A 与赛题指南属于正式依据；其他材料只有在被正式文档引用时才具有规则效力（第 1 页）。
- 延时由 SRB 内部、互联和特殊区域组成。当前互联基础延时按题目简化为 0，但 Gap 与可横向穿越 Block 会产生附加延时；Arc 延时固定。题目明确鼓励研究路径复用、层次化、模型和数据结构，逐请求传统寻路在超大规模下不可行（第 2 页）。
- `SRB_Net.json` 的相对偏移必须映射成真实端口连接；底层解析错误会使上层结果整体失效（第 3 页）。
- 只允许单线程，不允许 GPU 或多线程；30 分钟包含读入、预处理和计算（第 3 至 4 页）。
- Block 的 `lower/upper` 是物理 Y，`left/right` 是物理 X，不是 SRB 下标（第 4 页）。
- 公开 100 万和隐藏 1 亿请求均可达；隐藏集与公开集使用同一虚拟芯片结构和当前 Gap/Arc 配置，但隐藏点更多，且不保证包含公开 100 万 Golden（第 4 至 5 页）。
- 实现方式不受限，但内存属于正式约束（第 5 页）。

因此，V4 可以安全利用固定架构、端口家族、Gap 前缀和与有界 Atlas；不能把公开请求本身、公开 Golden 预测表或未经证明的平移等价关系当作隐藏集缓存。

## 2. 官方评分口径

对正 Golden：

```text
point_i = 1 - tanh(4 * |prediction_i - golden_i| / golden_i)
acc_score = 100 * mean(point_i)
time_score = 100 * (1 - average_seconds / 1800)
total = 0.80 * acc_score + 0.15 * time_score + 0.05 * consistency_score
```

正式运行 5 次；五次输出完全一致时 `consistency_score=100`，否则为 0。至少两次无效会使总分为 0。本文的总分均为“公开 100 万准确度 + 本机 1 亿条线性时间投影 + 一致性 100”的估算，不是隐藏 1 亿真实官方分数。

## 3. 公开请求对缓存与长路径复用的证据

全量 1,000,000 行解析成功，失败 0。关键统计如下：

| 方案 | 公开集证据 | 当前决策 |
|---|---:|---|
| Exact Query Cache | 1,000,000 个唯一请求，首见后命中率 0% | 保留接口价值，但公开分布下默认关闭 |
| Top-1000 Source Tree | 覆盖 0.3268% | 不值得预建大批完整 SPT |
| Top-1000 Target Tree | 覆盖 0.3090% | 同上 |
| 原始 `(portA,portB,dx,dy)` | 首见后重复上界 0.0560% | 只是几何相同，不安全复用 delay |
| 环境签名模板 | 首见后重复上界 0.0024% | 当前收益过低 |
| 保守安全模板 | 0% | 等价于绝对请求 |
| V3 Atlas | 实际覆盖 9.0182% | 保留 |
| Chebyshev <= 32 Macro 候选 | 5.4213% | 只有几何机会，未证明精确 |
| 无 Block 普通区域周期候选 | 52.2692% | 最值得继续做结构化长路径原型 |
| Block Portal 候选池 | 47.7308% | 需真实路径标签后再验证 |

请求的 Chebyshev 距离 P50/P90/P99 为 157/368/488，98.9632% 为斜向请求。公开分布确实以长路径为主，但绝对 Source/Target 重复很弱。因此“边搜索边记忆”的优先对象应是架构级局部状态、Portal 到 Portal 转移、周期单元转移和 Gap 前缀，而不是无界保存逐请求 A*/Dijkstra 结果。

## 4. Net 跨度 8/12 与 FPGA 重复结构

当前 `SRB_Net.json` 的最大实际跨度为 12，而 V3 声明上界为 8。Atlas 半径 56 来自 `48+8`；311 条 `fallback_other` 中，310 条残余 seed 跨度为 12、1 条为 10。现有 `LocalAtlas::get` 有边界检查，因此这是安全回退，不是越界。

直接把常量从 8 改成 12 不安全：需要半径 60 的新 Atlas，预计增加约 91.406 MiB，最多改变 311 条（0.0311%）。本轮不修改正式 Atlas。

更有价值的重复结构来自端口命名：496 个完整端口可由“去掉末尾十进制总线索引”的架构规则压缩成 216 个 family；320 个端口属于 40 个重复的 8-index family，176 个保持单例。family 映射只依赖 `SRB_Port.json`，不依赖 Golden。

## 5. Atlas 错误和失分贡献

Atlas 分支 90,182 行，占 9.0182%；其公开 `acc_score=96.226478`，同一批行改走 V3 fallback 只有 82.249694。完全关闭 Atlas 会使全量准确度从 95.308549 降到 94.048095，即 -1.260454。

Atlas 贡献总 point loss 的 7.2537%，低于其 9.0182% 行占比。Oracle 在 Atlas/fallback 二选一最多切换 5,303 行，将全量准确度提高到 95.436113，上界仅 +0.127563。无 Gap、仅水平 Gap、远边界和训练侧 Port-pair gate 均未在保留集产生正收益。因此本轮结论是保留 Atlas，不加入经验 gate。

## 6. single residual 热路径优化

新增 `--residual-kernel reference|optimized`，默认 `optimized`。优化版将边界距离与分箱改成构造期查表。修复 Y clearance 的 `uint8_t` 溢出后，100 万条输出与 reference 的 SHA-256 完全一致：

```text
BDC0F47A63EF55D0A71717779864EA9E05B8AAAA371E9A471252D794C6A2AAEC
```

10 轮测试中 reference 与 optimized 只差约 0.000284 秒/100 万，属于噪声级。进一步拆分表明 fused V4 context 相对 V3 约增加 0.003845 秒/100 万，compact residual 本身再增加约 0.018574 秒/100 万。主要成本是多表浮点残差，而不是 Block context 或边界分箱。优化正确但没有宣称已恢复到 V3 同速。

## 7. 压缩 Port-family 模型消融

所有消融均在七种固定切分中重训切分侧 V3，再只用训练侧拟合残差；验证侧不拟合 effect 或 alpha。

| 候选 | 参数 | float32 大小 | 七切分平均增益 | 最低/最高增益 | 公开全量诊断增益 |
|---|---:|---:|---:|---:|---:|
| 正式 compact single residual | 2,784 | 10.875 KiB | +0.054117 | +0.043842 / +0.074612 | +0.044836 |
| 完整 Port-pair | 408,512 | 1.559 MiB | +0.080632 | +0.051297 / +0.108210 | 研究态 |
| family-pair only | 48,448 | 189.25 KiB | +0.046165 | +0.035575 / +0.062335 | 未生成正式候选 |
| family compact | 48,880 | 190.94 KiB | +0.064047 | +0.052529 / +0.077502 | +0.057851 |
| family + family-distance | 118,432 | 462.63 KiB | **+0.084714** | **+0.059794 / +0.112199** | **+0.090352** |

完整 family C++ 与 Python 100 万行差异为 0；固定随机保留集增益 +0.065606。family compact 同样差异为 0，固定保留集增益 +0.048787。

## 8. 100 万性能与总分估算

同一 MinGW GCC 5.3、静态链接、同一输入和 Atlas；每个模式 10 轮。时间含查询和 CSV I/O，1 亿投影包含一次 Atlas 加载并按查询段线性外推。

| 模式 | 公开 acc | 100 万查询均值 | 1 亿投影 | 峰值 RSS | 估算总分 |
|---|---:|---:|---:|---:|---:|
| V3 | 95.308549 | 0.631264 s | 64.6053 s | 645.929 MiB | 95.708462 |
| 正式 compact V4 | 95.353385 | 0.657024 s | 67.1752 s | 645.953 MiB | 95.722915 |
| family compact V4 | 95.366401 | 0.651408 s | 66.6001 s | 646.071 MiB | 95.738119* |
| family-distance V4 | **95.398901** | 0.669430 s | 68.4240 s | 646.293 MiB | **95.748921** |

`*` family compact 的 V3/候选测速来自独立 10 轮配对，表中估算总分使用该组时间；不同组的毫秒级差异不可视为硬件稳定差异。

family-distance 当前是公开估算总分最优实验候选，比正式 compact V4 高约 +0.026006；但它仍继承使用公开 Golden 训练的 V3 基础模型，残差也来自公开训练侧。它不能证明隐藏集必然提升，暂不替换正式默认模型。

## 9. 对长路径搜索的当前工程决策

1. 保持 V3/compact V4 的 O(1) 长路径估算为默认兜底，避免逐请求图搜索。
2. Gap 继续使用架构前缀和，单请求 O(1)；公开请求 98.4696% 的端点跨度涉及 Gap，逐条扫描 Gap 不合算。
3. Atlas 作为一次预加载、多查询复用的局部最短路表保留；不根据公开 Golden 构造 gate。
4. 端口复用优先采用 architecture-derived family，而不是 496×496 完整 Port-pair；这证明了 FPGA 重复端口结构有可泛化的压缩价值。
5. 下一阶段的精确原型应面向无 Block 长路径构造“周期单元状态转移 + Gap 独立代价”，并用小图 Dijkstra 验证每个转移；Block 区域再接有界 Portal 图。
6. 若实现在线记忆，先做固定容量、架构指纹隔离的局部转移/Portal LRU，并记录命中率；不建立随 1 亿请求线性增长的绝对查询表。

## 10. 仍不能确定的事项

- 工作区没有隐藏 1 亿请求，因此不能得到真实全量官方总分或隐藏分布下的缓存命中率。
- 相同 `(port family, dx, dy, mod 8)` 不等于相同 delay；Block、Gap、边界与端口索引仍可能改变最短路。
- Net 最大跨度 12 不证明路由系统周期为 8、12 或其他数值。
- Block 包围盒相交不等于最短路径实际受 Block 影响；Portal 覆盖率仍缺真实路径标签。
- V5 式全局运行时预计算能复用架构搜索，但其公开准确度与 88.8 秒预处理表明不能直接替换 V4。
- 本机线性时间投影没有覆盖官方机器、文件系统、隐藏请求分布和五次运行波动。

## 11. 下一步优先实验

1. 无 Block 小规模精确 Dijkstra 对照：验证 8/12/16 周期单元转移及 Gap 可分离性。
2. 选择 1 至 2 个 Block 建立局部 Portal 原型，分别报告 bbox 候选、实际路径受阻和 exact 一致率。
3. 对 family-distance 表做 int16/分块布局实验，要求 C++ 输出误差、七切分分数和 1 亿投影总分三项共同改善。
4. 用精确小样本训练“算法选择器”，只决定 Atlas/周期/Portal/完整搜索，不直接预测 delay。
5. 若能获得官方 1 亿请求文件，先只跑无 Golden 的命中率、时间和内存审计，再计算真实官方分数。

## 12. 关键证据文件

- 全量查询统计：`analysis/public_queries_full/query_statistics.json`
- Atlas 失分：`analysis/srb_fast_v4_explore/atlas_loss/atlas_loss_analysis.json`
- family 七切分：`analysis/srb_fast_v4_explore/port_family_ablation/v4_validation.json`
- family compact 七切分：`analysis/srb_fast_v4_explore/port_family_compact_ablation/v4_validation.json`
- family 模型与分数：`analysis/srb_fast_v4_explore/family_model/`
- family compact 模型与分数：`analysis/srb_fast_v4_explore/family_compact_model/`
- family 10 轮性能：`analysis/srb_fast_v4_explore/family_benchmark_1m/benchmark_results.json`
- compact family 10 轮性能：`analysis/srb_fast_v4_explore/family_compact_benchmark_1m/benchmark_results.json`
- 默认 V4 回归输出：`analysis/srb_fast_v4_explore/regression_default_v4_1m.csv`

