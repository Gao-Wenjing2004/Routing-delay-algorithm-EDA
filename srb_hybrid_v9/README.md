# V9：紧凑精确搜索 + V8 回退

V9 已实现为可提交的单文件程序：少量由消融选中的请求进入确定性、最多
16 次状态展开的精确 A*；只有在预算内证明最短路时才采用精确值，否则立即
回退 V8 P2。长距离仍全部走 V8。

提交程序不携带 65 MB 全图，也不进行每查询反向 Dijkstra。`compact_graph_data.hpp`
只保存由官方图导出的端口、Arc、Net、Block/Gap 规则和端口状态下界；运行时
按规则生成邻边，并只记忆本次微型搜索访问的状态。

## 已测结果

- Linux 提交二进制：约 13.5 MB，低于 100 MB；
- 公开 1,000,000 条：V8 `94.603473234`，V9 `94.604410091`；
- 默认选择 10 条，10 条均在预算内精确完成，共展开 52 个状态；
- Windows 五轮百万条中位数：V8 `0.940940 s`，V9 `0.942671 s`；精确分支内部计时约 `0.2~0.4 ms / 1M`；
- 完整图 exact solver 对已知 31 条结果与旧 exact 基准 `31/31` 相同；
- 紧凑内核与完整图的“端口下界 + 16 展开”在全部 15,611 条公开短距离请求上，完成标志、展开数和精确值 `15,611/15,611` 相同。

准确度提升很小，V9 应视为“精确分支基础设施已成立”，不是已经达到 98% 的
终点。完整消融与下一步路径见 [V9_DESIGN_AND_FINDINGS.md](docs/V9_DESIGN_AND_FINDINGS.md)。

## 构建

Linux：

```bash
cd srb_hybrid_v9
./build.sh
```

Windows 上用 Zig 交叉编译：

```powershell
.\build_linux_with_zig.ps1 -ZigExe C:\path\to\zig.exe
```

产物是 `submission/bin/estimate`，调用方式仍为：

```bash
./estimate -in request.csv -out result.csv -threads 1
```

## 生成额外训练请求

只能组合官方存在的 SRB 单元和 496 种合法端口，不能发明新端口语义：

```bash
python3 tools/generate_extra_queries.py \
  --inst arch/SRB_Inst.json --port arch/SRB_Port.json \
  --count 100000 --max-cheb 32 --output extra_requests.csv
```

再用完整图研究版的无界 exact 模式离线标注；只有通过已知 Golden 一致性验证的
求解器才可充当 teacher。生成数据与公开验证集必须隔离。
