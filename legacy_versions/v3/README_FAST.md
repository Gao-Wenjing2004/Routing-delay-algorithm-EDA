# SRB Fast V3

V3 保留 V2 的局部最短路 Atlas，并重点增强距离超过 48 格的长路径回退模型。

## 算法

- 普通局部请求：查询半径 56 的 `local_atlas.bin`，加上端口 Arc 和必经 Gap Line 延时。
- 长距离或受 Block 影响的请求：使用 O(1) 长路径模型，不运行 A*/Dijkstra。
- V3 新增长距离特征：源-目标端口对、端口与距离组合、端口与跨度余数组合、细粒度位移、方向比例和芯片区域。
- Block 的端点相对位置参与模型；当前仍是估算，不是 Block 拓扑上的精确最短路。

## Windows：构建

```powershell
cd "C:\Users\ZC\Desktop\紫光tc\srb_fast_v3"
powershell -ExecutionPolicy Bypass -File .\build_fast.ps1 -SkipTrain
```

`-SkipTrain` 表示使用已经生成的 `fast_model_data.hpp`。如果需要重新用公开 Golden 训练：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_fast.ps1
```

## 运行31条样例并看准确率

```powershell
powershell -ExecutionPolicy Bypass -File .\run_fast.ps1 `
    -InputCsv .\arch\queries.csv `
    -OutputCsv .\output\v3_predictions_31.csv

python .\validate_fast_model.py `
    .\arch\queries.csv `
    .\output\v3_predictions_31.csv
```

主要看验证输出中的 `acc_score`、`mean relative error` 和 `within 5%`。

## 运行100万公开数据并看准确率

```powershell
.\estimate.exe `
    -in .\arch\delay_estimate_ans.csv `
    -out .\output\v3_predictions_1m.csv

python .\validate_fast_model.py `
    .\arch\delay_estimate_ans.csv `
    .\output\v3_predictions_1m.csv
```

`estimate.exe` 输出中的：

- `local_queries`：使用局部 Atlas 的数量；
- `fallback_queries`：使用 V3 长路径模型的数量；
- `elapsed`：CSV 查询阶段耗时，不包含 Atlas 加载；
- `throughput`：每秒处理请求数。

## 当前结果

| 指标 | V2 | V3 |
|---|---:|---:|
| 100万公开集 `acc_score` | 94.218939 | **95.308549** |
| 长距离得分 | 94.225695 | **95.378979** |
| 长距离普通区域 | 93.181926 | **94.496441** |
| 长距离 Block 区域 | 95.211130 | **96.212194** |
| 完全匹配率 | 8.9097% | **9.1769%** |
| 平均相对误差 | 1.4940% | **1.2126%** |
| 误差不超过5% | 95.0524% | **96.4691%** |
| 100万条查询阶段 | 约1.5-2.5秒 | **1.690秒** |

固定80/20留出验证中，纯回退模型由 V1 的约92.54提升到 **93.464148**，说明新增特征不只是记住全量公开结果。但公开集仍参与最终模型训练，隐藏集成绩以正式评测为准。

## 赛题接口

```powershell
.\estimate.exe -in .\delay_estimate_request.csv -out .\delay_estimate_result.csv
```

运行时需要将 `local_atlas.bin` 放在 `estimate.exe` 同目录。
