# SRB PRP-R V8（第一轮）

这是独立于 V3/V4/V7 的无 Atlas 研究分支。当前实现完成 PRP-R 第一轮：周期商图、自动原语发现、`P0_no_atlas` 对照、纯结构 `P1_primitive_base`、百万条误差分析，以及小于 100MB 的正式 C++ 流式程序。

当前结论不是“P1 已可提交争取高分”。P1 的体积和速度达标，但准确度只有 68.817773；应把它保留为结构基线，并进入 P2 低秩端口修正与 P3 余数校正。

## 目录

- `tools/prpr_arch.py`：读取官方 JSON，构建 160 状态周期商图并发现原语。
- `tools/build_prpr_assets.py`：生成商图、原语、连接器和分析模型。
- `tools/export_cpp_model.py`：将小型结构模型导出为 C++ 头文件。
- `training/evaluate_first_stage.py`：在固定 FNV 20% 留出和完整公开集上比较 P0/P1/V3。
- `src/prpr_estimator.hpp`：无 Atlas、固定候选数的 C++ P1 推理核心。
- `src/estimate.cpp`：面向大 CSV 的单线程流式入口。
- `docs/prpr_design.md`：设计与边界说明。
- `analysis/prpr/first_stage_conclusion.md`：第一轮数据和 Go/No-Go 结论。

## 复现实验

在仓库根目录执行，按本机实际位置替换官方数据路径：

```powershell
py -3.14 srb_prpr_v8/tools/build_prpr_assets.py `
  --arch-dir ../plusone-srb_fast_v3/srb_fast_v3/arch

py -3.14 srb_prpr_v8/tools/export_cpp_model.py

py -3.14 srb_prpr_v8/training/evaluate_first_stage.py `
  --golden ../plusone-srb_fast_v3/srb_fast_v3/arch/delay_estimate_ans.csv `
  --p0 ../analysis/srb_fast_v4/atlas_disabled_v3_predictions.csv `
  --v3 ../plusone-srb_fast_v3/srb_fast_v3/output/v3_predictions_1m.csv
```

Linux 常规构建：

```bash
cd srb_prpr_v8
./build.sh
./submission/bin/estimate -in request.csv -out result.csv
```

也可在 Windows 上用 `build_linux_with_zig.ps1 -ZigExe <zig.exe>` 交叉编译静态 Linux x86-64 程序。编译参数未使用 `-march=native`。

## 当前产物

- Linux 静态二进制：`submission/bin/estimate`，8,165,056 bytes。
- Windows 本地性能二进制：1,033,216 bytes（`build/` 被 Git 忽略）。
- 嵌入模型头：197,497 bytes。
- 外部 Atlas：0 bytes。

百万请求的五次本地输出 SHA-256 完全一致。详细数据见 `analysis/prpr/benchmark_results.json`。
