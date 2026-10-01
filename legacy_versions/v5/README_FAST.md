# SRB Fast V5：单文件、运行时预处理版

V5 现在只提交一个 `estimate.exe`，不携带 `local_atlas.bin`、`arch_geometry.bin` 或 `arch_model.bin`。程序启动后在内存中完成全部派生预处理，打印的 `total_elapsed` 包含预处理、读取请求、计算和写出结果。

## 算法思路

编译前，`prepare_local_arch.py` 只把官方 Port、Arc、Net、Gap、Block JSON 转成约 320 KiB 的原始架构常量 `local_arch_data.hpp`，再编进程序。它不是 Golden 训练结果，也不包含预先求好的最短路。

`estimate.exe` 每次启动后依次执行：

1. 对 160 个 routing Input 做一次全局多源 Dijkstra，运行时生成覆盖 `dx=[-119,119]`、`dy=[-549,549]` 的长路径 Geometry。
2. 对 160 个 routing Input 分别执行局部 Dijkstra，运行时生成半径 56 的精确局部 Atlas。
3. 仅从刚生成的 Atlas 在 24、34、44 三个尺度推导 9 个方向的端口进入/退出瞬态。
4. 处理请求：距离不超过 48 且包围盒不接触 Block 时查局部 Atlas；其余请求使用全位移 Geometry 加端口瞬态。
5. Gap Line 用前缀和加入延时；可横穿 Block 加穿越代价；不可纵穿 Block 使用共享横向绕行代价。

V5 不读取公开 100 万条 Golden 数据来训练、拟合或建立索引。公开答案只用于完成程序后的只读评分验证。

## 完整运行实测（包含预处理）

本机完整 100 万条：

| 指标 | 结果 |
|---|---:|
| Geometry 预处理 | 约 10.6 秒 |
| Atlas 预处理 | 约 72.2 秒 |
| 端口模型推导 | 约 0.4 秒 |
| 全部预处理 | **83.18 秒** |
| 100 万条查询和写出 | **1.71 秒** |
| 整进程总时间 | **84.90 秒** |
| 实测峰值工作集（含预处理） | **656.7 MiB** |
| 持久核心数组 | 628.2 MiB |

公开集只读评分：

| 指标 | 结果 |
|---|---:|
| `acc_score` | **92.373770** |
| 平均相对误差 | 1.9818% |
| 中位相对误差 | 1.1367% |
| 误差不超过 5% | 91.7261% |
| 完全匹配 | 88,983（8.8983%） |

运行时生成版与旧文件加载版的 100 万条输出 SHA-256 完全相同：

```text
24732994FBFE58E0CBE55584095D72A1555D54A3FB9F28F5857CD229862A9567
```

因此在本机实测下，完整流程满足单线程、确定性、2 GiB 内存和 30 分钟总时限。若评测请求扩大到 1 亿条，预处理仍固定约 83 秒；按当前纯查询吞吐外推，计算与 CSV I/O 还需约 171 秒，总计约 4.2 分钟。该数字会随评测机和磁盘速度变化。

## Windows 构建与运行

```powershell
cd "C:\Users\ZC\Desktop\紫光tc\srb_fast_v5"
powershell -ExecutionPolicy Bypass -File .\build_v5.ps1
.\estimate.exe -in .\delay_estimate_request.csv -out .\delay_estimate_result.csv
```

也可以使用包装脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\run_v5.ps1 `
  -InputCsv .\delay_estimate_request.csv `
  -OutputCsv .\delay_estimate_result.csv
```

创建纯净提交目录：

```powershell
powershell -ExecutionPolicy Bypass -File .\make_submission.ps1
```

生成的 `submission_windows` 中只有 `estimate.exe`。

## Linux

```bash
chmod +x build_v5.sh run_v5.sh
./build_v5.sh
./run_v5.sh ./delay_estimate_request.csv ./delay_estimate_result.csv
```

Linux 正式提交前仍需在与赛方评测环境一致的发行版和编译器上重新编译、检查接口并实测总时间和峰值内存。
