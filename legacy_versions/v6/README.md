# SRB Fast V6

V6 是在已验证 V3/V4 解析器和常数时间基线上实现的独立实验版本。它吸收了“先结构分流、再选择 Atlas / 周期残差 / 兜底”的思路，但没有把未经验证的 Template Cache、在线最短路或 Portal Graph 接入正式预测路径。

## 当前正式 V6 路径

1. 使用 V3/V4 的零分配端点解析、同端点规则、Gap 前缀和、Block 上下文和资产指纹校验。
2. 无 Block 且局部可达的请求查询 legacy Atlas；V6 将查询半径从 48 扩大到 56。公开 100 万条上的 Atlas 分支为 109,273 条（另有 1 条同端点），占 10.9273%。
3. 其余请求使用 V3 的 O(1) 基础预测，加一个 Macro10 + port-family + 绝对相位残差。残差共 205,932 个 float32 参数（823,728 字节）。
4. 为减少每条查询的取模开销，x/y 相位和 dx/dy 余数使用边界内查表；优化前后百万条输出逐字节一致。

Macro10 是公开数据消融后得到的经验周期候选，不代表 FPGA 路由图存在严格 10×10 平移不变性。family 映射来自架构端口命名；残差参数由公开 Golden 的固定训练侧拟合，因此 V6 不是纯精确算法，也不能由公开结果证明隐藏集一定提升。

## 构建

Windows：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\srb_fast_v6\build_v6.ps1
```

Linux：

```bash
bash ./srb_fast_v6/build_v6.sh
```

构建脚本会校验架构指纹、Atlas CRC32/SHA256、模型头文件哈希和参数数量。V6 复用 V3 的 `local_atlas.bin`，该文件不重复存入本目录。

## 运行

```powershell
.\srb_fast_v6\estimate_v6.exe `
  -in .\delay_estimate_request.csv `
  -out .\delay_estimate_result.csv `
  -atlas .\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
  --mode v6
```

同一个 V6 可执行文件还支持 `--mode v3`、`--mode v4` 和 `--mode v4-base`，便于同机回归比较。V3/V4 模式保持半径 48；V6 默认半径 56。可用 `--atlas-query-radius 0..56` 做消融。

## 已测结果（公开 100 万条）

| 版本 | `acc_score` | MAE/ps | 1 亿条线性投影/s | 估算总分 |
|---|---:|---:|---:|---:|
| V3 | 95.308549 | 29.955889 | 61.082756 | 95.737817 |
| compact V4 | 95.353385 | 29.638363 | 62.479325 | 95.762047 |
| V6 | 95.637129 | 28.183614 | 67.521067 | 95.947028 |

时间来自同一 V6 可执行文件、同一机器、每模式 10 轮的 `query_and_io` 均值；三种模式各自 10 轮输出一致。总分是“公开准确度 + 本机线性时间投影 + 一致性 100”的估算，不是隐藏 1 亿条官方真实成绩。

完整实验、分段结果、消融和限制见 [`../docs/srb_fast_v6_report.md`](../docs/srb_fast_v6_report.md)。
