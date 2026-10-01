# SRB Fast V4

V4 是独立于 V3 的最小增量实现。它保留 V3 的端点解析、局部 Atlas 行为和 O(1) 基础模型，只对 V3 fallback 请求增加一个经过固定划分验证的轻量相对残差模型。

重要：100 万公开 Golden 审计表明 V3 Atlas 命中并非逐条精确，因此代码和日志统一称其为 `legacy_atlas`，不标成 Exact。完整审计见 `../analysis/srb_fast_v4/v3_audit.json`。

## 构建

```powershell
powershell -ExecutionPolicy Bypass -File .\build_v4.ps1 -BuildV3Parity
```

只有需要按锁定结构重新拟合最终参数时才添加 `-TrainModel`。训练不会覆盖 V3 模型或 Atlas。

## 运行与 V3 对比

```powershell
.\estimate_v4.exe `
    -in ..\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
    -out .\v4_predictions.csv `
    -atlas ..\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
    --mode v4

.\estimate_v4.exe `
    -in ..\plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv `
    -out .\v3_mode_predictions.csv `
    -atlas ..\plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin `
    --mode v3
```

`--mode v3` 关闭 V4 残差并用于同一二进制、同一编译参数下的公平对比。`--mode v4-base` 是诊断模式，用于证明融合 fallback 与 V3 逐条等价。`--disable-atlas` 可关闭未证明精确的 Atlas 实验分支。构建前校验架构、V3 推理头、残差头和 Atlas 资产，运行时再校验 Atlas CRC32、头和文件大小；SHA-256 与训练元数据记录在 `v4_model_manifest.json`。

## 当前正式主路径

1. 无分配端点解析与完整端口 ID；
2. 同端点严格特例；
3. 可配置的 V3 legacy Atlas；
4. 与 V3 输出逐字节等价、同时复用 Block 上下文的 O(1) fallback；
5. fallback 上 7 次小型常量表查询的 `single_residual`（2,784 个 float）；
6. 非负、最近整数输出。

Exact Cache、Source/Target Tree、Template Cache、Macro、min-plus 和 Block Portal 均未接入。

408,512 参数的 `port_pair_residual` 虽有更高准确性增益，但不同规模的耗时证据不能保证最弱留出集总分为正，故只保留在消融结果中。最终选择依据见 `../analysis/srb_fast_v4/v4_final_selection.json`。
