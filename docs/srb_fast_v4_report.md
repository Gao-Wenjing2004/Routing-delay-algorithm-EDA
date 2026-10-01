# SRB Fast V4 最终实现与验证报告

日期：2026-08-28。结论：正式 V4 采用 **2,784 参数 compact 单残差**，保留 V3 legacy Atlas 和 V3 fallback 语义；在七类训练/验证隔离场景中准确性分均提升，计入实测时间项后的预计总分增益也均为正。408,512 参数端口对残差因性能证据不稳定而保持研究态。

本报告中的“预计总分”使用公开 Golden 的准确性和本机线性时间外推，不是官方隐藏集成绩，也不是正式 1 亿实测。

## 1. 官方评分公式与硬性限制

- 正式输入：`delay_estimate_request.csv`，1 亿条 `From,To` 请求；端点格式 `SRB_x_y/PORT`。
- 输出：与输入同序的 `From,To,delay`；V4 输出非负整数 ps，解析失败时非零退出，不静默丢行。
- 时间限制：单次 1800 秒；官方运行 5 次。
- 内存限制：峰值 2 GB。
- 总分：`0.80 * acc_score + 0.15 * time_score + 0.05 * consistency_score`。
- 对正 Golden：`point_i = 1 - tanh(4 * abs(pred_i-golden_i)/golden_i)`，`acc_score=100*mean(point_i)`。
- `time_score = 100 * (1 - avg_time/1800)`。
- 五次输出完全一致时一致性分 100，否则为 0，且准确性取五次平均。
- 五次中至少两次超时、超内存或其他无效情况，总分为 0。

官方资料未明确：编译器/语言/第三方依赖、离线 Atlas/权重是否允许、公开 Golden 是否允许训练、提交包体积、零 Golden、坏行和恰好一次无效运行的处理。详见 `docs/v4_score_contract.md`，本地评分器把未定义项参数化，没有补造官方规则。

## 2. V3 与正式精确基线

### V3 公开全量成绩

同一最终 V4 二进制的 `--mode v3` 在公开 100 万条上的 C++ 结果：

| 指标 | V3 |
|---|---:|
| 官方 `acc_score` | 95.308549 |
| MAE | 29.955889 ps |
| RMSE | 51.970348 ps |
| 平均相对误差 | 1.212550% |
| P50/P90/P95/P99 绝对误差 | 20 / 61 / 87 / 214 ps |
| 最大绝对误差 | 809 ps |
| 完全一致 | 91,770 |

`--mode v3` 与用同一 GCC 5.3、同一参数独立编译的原 V3 在 100 万条上逐字节一致。与项目中历史 V3 输出相比有 287 条（0.0287%）相差 ±1 ps，来源是编译环境的浮点取整差异；同编译环境公平比较没有该差异。

### 正式精确基线

正式 A* 基线在 31 条官方示例上逐条等于 Golden；本机单次受控测量为 24.817 秒、1.249 q/s、峰值 RSS 224.328 MiB。直接线性外推到 1 亿为 80,054,839 秒，仅作诊断；31 条查询混合不能代表正式数据。由此没有继续执行约 22 小时的 10 万或约 9 天的 100 万精确基线长跑。

### V3 Atlas 审计

V3 的 local 分支包含 90,181 条 legacy Atlas 请求和 1 条同端点请求；原 V3 计数合计 90,182。公开 Golden 上这 90,182 条中仅 80,318 条完全一致（89.0621%），`acc_score=96.226478`、MAE 13.2677 ps、P95 133 ps、最大误差 789 ps。因此它保留为兼容分支，但不得标记为 Exact。

## 3. V4 最终框架

```text
CSV 行
  -> 复用 V3 的无分配 Slice 解析和完整端口 ID
  -> 同一绝对端点（strict exact）
  -> 可关闭的 V3 legacy Atlas（estimated，不标 Exact）
  -> 与 V3 输出逐字节等价的融合 fallback
  -> 7 个小型表的相对残差
  -> 非负裁剪、最近整数、原顺序输出
```

融合 fallback 在不改 V3 文件的前提下复写 V3 固定公式，并在 V3 原有 7-Block effect 循环内同时收集残差所需 Block 包围盒计数。`--mode v4-base` 在公开 100 万条上与 `--mode v3` 输出逐字节一致。

正式残差公式：

```text
delay_v4 = round(max(0, base * (1 + 1.25 * sum(effect_j[code_j]))))
```

七个表为：预测值分箱、方向/距离分箱、边界距离、Gap 数量组合、Block 数量/方向/距离、完整 source port、完整 target port。Block/Gap 都只是可学习特征，不宣称精确路径修正。模型共有 2,784 个 float，11,136 字节。

正式残差只使用固定随机分层的 800,304 条训练行，其中 fallback 训练行 728,040 条；199,696 条固定留出没有参与最终 effect 拟合。该留出上的准确性分相对既有 V3 提升 +0.043653。需要注意，继承的 V3 基础模型此前已使用全部公开 Golden 拟合，因此这仍不是完全无泄漏的隐藏集估计；新 V4 参数本身满足不使用固定留出拟合的要求。

## 4. 新增与修改文件

正式 V4 独立目录：

- `srb_fast_v4/estimate_v4.cpp`
- `srb_fast_v4/srb_v4.hpp`
- `srb_fast_v4/train_v4_residual.py`
- `srb_fast_v4/v4_residual_data.hpp`（生成）
- `srb_fast_v4/v4_model_manifest.json`（生成）
- `srb_fast_v4/build_v4.ps1`
- `srb_fast_v4/build_v4.sh`
- `srb_fast_v4/Makefile.srb`
- `srb_fast_v4/README.md`

规则、设计与报告：

- `docs/v4_score_contract.md`
- `docs/srb_fast_v4_design.md`
- `docs/srb_fast_v4_report.md`

研究、评分和验证工具：

- `tools/srb_score.py`、`tools/test_srb_score.py`
- `tools/srb_v4_research.py`
- `tools/benchmark_srb_v4.py`
- `tools/benchmark_srb_baseline.py`
- `tools/compare_srb_predictions.py`
- `tools/summarize_srb_v4_results.py`
- `tools/verify_srb_v4_assets.py`

没有修改或覆盖 `plusone-srb_fast_v3/srb_fast_v3/` 中的 V3 模型、Atlas 或求解源码。

## 5. Net 跨度 8/12 调查

`SRB_Net.json` 的实际最大跨度为 12，V3 声明常量为 8，Atlas 半径为 `48+8=56`。公开数据的 311 条 `fallback_other` 全部是 source seed 后的残余位移超出 56；230 个 seed 跨度为 12，81 个为 10。现有 `LocalAtlas::get` 有边界检查，所以是安全回退，不是越界读表。

只把常量改成 12 不安全：要求 Atlas 半径变为 60，而当前 Atlas 会被拒绝。半径 60 预计需要 749,619,240 字节，比当前增加 95,846,400 字节（91.406 MiB），最多覆盖 311 条（0.0311%）。该修改不进入 V4。

## 6. 特征与模型消融

所有划分都在训练侧重拟 V3，再仅用 fallback 训练行拟合残差；验证侧不用于拟合表或 alpha。

| 候选 | 参数 | 七划分平均准确性增益 | 最小 | 最大 | 决策 |
|---|---:|---:|---:|---:|---|
| 全局校准 | 68 | +0.00721 | -0.00602 | +0.03275 | 关闭，不稳定 |
| compact 单残差 | 2,784 | +0.05412 | +0.04384 | +0.07461 | 正式默认 |
| Block 双专家 | 3,024 | +0.05556 | +0.04592 | +0.06914 | 关闭，未证明分支价值 |
| 边界三专家 | 3,776 | +0.05466 | +0.04761 | +0.06844 | 关闭，未优于双专家 |
| 端口对残差 | 408,512 | +0.08063 | +0.05130 | +0.10821 | 研究态，耗时不稳定 |

端口对版本的 100K、1M、5M 外推额外时间分别为 3.098、5.637、0.139 秒，最大观测开销会使最弱切分预计总分变化为 -0.00594，故未因更高准确性而上线。compact 的对应最大观测开销为 2.758 秒，最弱切分保守总分增益仍为 +0.01209。

## 7. 七类验证集 V3/V4 对比

下表使用最终模型 5x5M 测得的共同时间假设（V3 63.6222 秒、V4 66.2127 秒）计算预计总分。V3 是每个划分训练侧重拟后的基准。

| 划分 | 验证行 | V3 acc | V4 acc | acc 增益 | 预计总分增益 | V3/V4 MAE |
|---|---:|---:|---:|---:|---:|---:|
| 随机分层 | 199,696 | 94.840259 | 94.885261 | +0.045002 | +0.014414 | 33.259 / 32.939 |
| 空间区域 | 357,905 | 94.695364 | 94.742631 | +0.047267 | +0.016225 | 33.579 / 33.246 |
| 长距离 | 100,530 | 96.847212 | 96.921823 | +0.074612 | +0.038101 | 44.072 / 43.014 |
| Block 留出 | 477,308 | 94.439423 | 94.491065 | +0.051643 | +0.019726 | 39.500 / 38.970 |
| Gap 分层 | 200,493 | 94.820308 | 94.864149 | +0.043842 | +0.013485 | 33.287 / 32.979 |
| 稀有端口对 | 106,727 | 94.035296 | 94.089139 | +0.053843 | +0.021486 | 38.758 / 38.381 |
| 边界留出 | 318,888 | 95.478858 | 95.541467 | +0.062609 | +0.028499 | 33.006 / 32.455 |

完整 RMSE、相对误差、P50/P90/P95/P99、最大误差以及距离、Block、Atlas、稀有端口分段指标保存在 `v4_validation.json` 和 `v4_validation_summary.csv`。

## 8. 运行时间与正式规模外推

本机编译器：MinGW-w64 GCC 5.3，`-std=c++1z -O3 -DNDEBUG`，静态链接；V3/V4 使用同一可执行文件、输入、Atlas 和编译参数，运行顺序逐对交错。

| 规模/模式 | 次数 | Atlas 加载均值 | 查询+I/O 均值 | wall 均值 | QPS 均值 |
|---|---:|---:|---:|---:|---:|
| 100K V3 | 5 | 1.4635 s | 0.065364 s | 1.6208 s | 1,530,164 |
| 100K V4 | 5 | 1.4540 s | 0.066998 s | 1.5630 s | 1,493,242 |
| 1M V3 | 5 | 1.4546 s | 0.631870 s | 2.1275 s | 1,582,799 |
| 1M V4 | 5 | 1.4529 s | 0.659471 s | 2.1522 s | 1,516,499 |
| 5M 重复输入 V3 | 5 | 1.4599 s | 3.108115 s | 4.6227 s | 1,608,707 |
| 5M 重复输入 V4 | 5 | 1.4583 s | 3.237721 s | 4.7529 s | 1,544,387 |

以更长的 5M 测量区间线性外推 1 亿：V3 63.6222 秒，V4 66.2127 秒，均远低于 1800 秒。5M 文件是把公开 1M 顺序重复五次，目的仅是提高计时信噪比；它不模拟隐藏查询分布，也不等于正式 1 亿实测。

## 9. 峰值内存与资产体积

| 项目 | 数值 |
|---|---:|
| Atlas 文件/主数组 | 653,772,840 B（623.486 MiB） |
| V3 float 参数 | 3,987,800 B（3.803 MiB） |
| V4 float 参数 | 11,136 B（10.875 KiB） |
| V4 静态可执行文件 | 6,808,535 B |
| 1M V3 峰值 RSS | 645.916 MiB |
| 1M V4 峰值 RSS | 645.948 MiB |

距离 2 GB 限制约有 1.35 GiB 余量。输入和输出均使用固定 4 MiB 缓冲，不随请求数量增长缓存或 `unordered_map`。

## 10. 预计官方成绩

公开 100 万 C++ 输出（混合 800,304 条新残差训练行和 199,696 条固定留出行）：

| 指标 | V3 | V4 | 变化 |
|---|---:|---:|---:|
| `acc_score` | 95.308549 | 95.353385 | +0.044836 |
| MAE | 29.955889 | 29.638363 | -0.317526 ps |
| RMSE | 51.970348 | 52.080542 | +0.110194 ps |
| 平均相对误差 | 1.212550% | 1.201480% | -0.011070 pct-pt |
| P50/P90/P95/P99 | 20/61/87/214 | 19/60/86/214 | 改善/持平 |

将上述公开同数据准确性、5M 时间外推和五次一致性假设代入官方公式：

| 项目 | V3 | V4 |
|---|---:|---:|
| 预计 1 亿时间 | 63.6222 s | 66.2127 s |
| `time_score` | 96.465436 | 96.321515 |
| `consistency_score` | 100 | 100 |
| 预计总分 | 95.716655 | **95.730935** |

预计总分增益为 **+0.014280**。该值很小，必须在官方硬件上重测；因此 `--mode v3` 是正式保留的回退，而不是仅调试代码。

## 11. 进入正式主路径的模块

- V3 无分配端点解析与完整端口 ID。
- 同一绝对端点 strict exact 特例。
- V3 legacy Atlas，保留原行为、可关闭、明确标为 estimated。
- 与 V3 逐字节等价的融合 O(1) fallback。
- 2,784 参数 compact relative residual，仅用于 fallback。
- 非负和最近整数输出。
- 构建期架构/V3 推理头/残差头/Atlas 指纹校验，运行期 Atlas 头、大小和 CRC32 校验。

## 12. 关闭或保持研究态的模块

- Exact Query Cache：公开重复率 0%。
- Source/Target Tree：Top-1000 仅覆盖 0.3268%/0.3090%。
- 位移 Template Cache：原始上界 0.0560%，安全模板 0%。
- 8/16/32 Macro、min-plus：32 范围候选仅 5.4213%，无精确性/总分闭环。
- Block Portal：Block 包围盒不是实际最短路径影响标签。
- 大 Net-span Atlas：91.406 MiB 换最多 311 条。
- 全局校准：至少一个切分回归。
- 双/三专家：收益不足以证明分支复杂度。
- 端口对残差：准确性更好，但耗时测量不能稳定保证最弱切分总分为正。
- 新 Direct Arc/单 Net exact 分支：尚未完成完整规则证明和覆盖收益验证。

## 13. 完整复现命令

以下命令从工作区根目录执行；Windows Python 路径可替换为环境中的 Python。

### 固定切分、V3 审计与消融

```powershell
python .\tools\srb_v4_research.py `
  --golden .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  --v3-predictions .\plusone-srb_fast_v3\srb_fast_v3\output\v3_predictions_1m.csv `
  --arch-dir .\plusone-srb_fast_v3\srb_fast_v3\arch `
  --atlas-file .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
  --v3-train-script .\plusone-srb_fast_v3\srb_fast_v3\train_fast_model.py `
  --output-dir .\analysis\srb_fast_v4
```

`v4_validation.json` 中的 `selected_candidate=port_pair_residual` 是只看准确性的预选择；正式 score-aware 决策由后续 `v4_final_selection.json` 覆盖。

### 锁定 compact 模型、校验资产与构建

```powershell
python .\srb_fast_v4\train_v4_residual.py `
  --golden .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  --v3-predictions .\plusone-srb_fast_v3\srb_fast_v3\output\v3_predictions_1m.csv `
  --arch-dir .\plusone-srb_fast_v3\srb_fast_v3\arch `
  --atlas-file .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
  --fixed-splits .\analysis\srb_fast_v4\v4_fixed_splits.npz `
  --out-expected .\analysis\srb_fast_v4\v4_compact_expected_predictions.npy

powershell -ExecutionPolicy Bypass -File .\srb_fast_v4\build_v4.ps1 -BuildV3Parity
```

### 运行 V4 与同二进制 V3

```powershell
.\srb_fast_v4\estimate_v4.exe `
  -in .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  -out .\analysis\srb_fast_v4\v4_predictions.csv `
  -atlas .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
  --mode v4

.\srb_fast_v4\estimate_v4.exe `
  -in .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  -out .\analysis\srb_fast_v4\v3_predictions.csv `
  -atlas .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
  --mode v3
```

### 100K/1M 五次成对 benchmark

```powershell
python .\tools\benchmark_srb_v4.py `
  --executable .\srb_fast_v4\estimate_v4.exe `
  --input .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  --atlas .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
  --output-dir .\analysis\srb_fast_v4\benchmark_final `
  --sizes 100000,1000000 --runs 5 --modes v3,v4
```

### 5M 长区间 benchmark、评分与最终选择

```powershell
python .\tools\benchmark_srb_v4.py `
  --executable .\srb_fast_v4\estimate_v4.exe `
  --input .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  --atlas .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
  --output-dir .\analysis\srb_fast_v4\benchmark_5m_final `
  --sizes 5000000 --runs 5 --modes v3,v4

python .\tools\srb_score.py `
  .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  .\analysis\srb_fast_v4\benchmark_final\v3_1000000_predictions.csv `
  --average-seconds 63.6221588 `
  --json-out .\analysis\srb_fast_v4\benchmark_final\v3_official_estimate.json

python .\tools\srb_score.py `
  .\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
  .\analysis\srb_fast_v4\benchmark_final\v4_1000000_predictions.csv `
  --average-seconds 66.2127294 `
  --json-out .\analysis\srb_fast_v4\benchmark_final\v4_official_estimate.json

python .\tools\summarize_srb_v4_results.py `
  --validation .\analysis\srb_fast_v4\v4_validation.json `
  --compact-final-benchmark .\analysis\srb_fast_v4\benchmark_final\benchmark_results.json `
  --compact-5m-benchmark .\analysis\srb_fast_v4\benchmark_5m_final\benchmark_results.json `
  --port-pair-final-benchmark .\analysis\srb_fast_v4\benchmark\benchmark_results.json `
  --port-pair-5m-benchmark .\analysis\srb_fast_v4\benchmark_5m_port_pair\benchmark_results.json `
  --v3-score .\analysis\srb_fast_v4\benchmark_final\v3_official_estimate.json `
  --v4-score .\analysis\srb_fast_v4\benchmark_final\v4_official_estimate.json `
  --out-csv .\analysis\srb_fast_v4\v4_validation_summary.csv `
  --out-json .\analysis\srb_fast_v4\v4_final_selection.json
```

### 回归测试

```powershell
python -m unittest tools.test_srb_score tools.test_analyze_public_queries -v
```

当前结果：11 项测试全部通过。
