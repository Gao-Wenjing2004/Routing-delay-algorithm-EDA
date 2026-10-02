# SRB PRP-R V8

这是独立于 V3/V4/V7 的无 Atlas 研究分支。当前实现包含 P1 周期路由原语研究，以及可提交试测的 P2 小模型。

P1 的体积和速度达标，但准确度只有 68.817773，不能直接提交。P2 改用无 Atlas V6 回退作为数值基线，用离线 Dijkstra Teacher 和 Golden 固定训练侧蒸馏出 8+8 棵小树，再加三个 Q20 固定模板；公开全量 C++ 准确度 94.603474，固定验证 94.485992，亿条本机线性投影 101.591 秒。

## 目录

- `tools/prpr_arch.py`：读取官方 JSON，构建 160 状态周期商图并发现原语。
- `tools/build_prpr_assets.py`：生成商图、原语、连接器和分析模型。
- `tools/export_cpp_model.py`：将小型结构模型导出为 C++ 头文件。
- `training/evaluate_first_stage.py`：在固定 FNV 20% 留出和完整公开集上比较 P0/P1/V3。
- `src/prpr_estimator.hpp`：无 Atlas、固定候选数的 C++ P1 推理核心。
- `src/estimate.cpp`：面向大 CSV 的单线程流式入口。
- `docs/prpr_design.md`：设计与边界说明。
- `analysis/prpr/first_stage_conclusion.md`：第一轮数据和 Go/No-Go 结论。
- `training/train_p2_student.py`：固定留出、Teacher/Golden 两阶段训练和消融。
- `tools/export_p2_trees.py`：把选定 LightGBM 小树量化导出成 C++ 头。
- `p2_runtime/`：P2 无 Atlas 单线程正式运行时。
- `analysis/p2/README.md`：P2 结果、边界与记忆策略总说明。

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

# P2 训练需要 numpy、scikit-learn、lightgbm；正式候选参数如下
py -3.14 srb_prpr_v8/training/train_p2_student.py `
  --golden ../baseline-srb_solver/srb_solver/arch/delay_estimate_ans.csv `
  --arch-dir ../plusone-srb_fast_v3/srb_fast_v3/arch `
  --base ../analysis/srb_fast_v6/atlas_radius_ablation/radius_0.csv `
  --teacher ../analysis/srb_fast_v6/atlas_radius_ablation/radius_56.csv `
  --trees 8 --lgb-learning-rate 0.5 `
  --candidates port_family_macro10_family_phase `
  --post-candidates port_family_macro10_family_phase `
  --post-group-subset source_family_distance,source_family_macro8,macro8_direction `
  --output-dir srb_prpr_v8/analysis/p2_memory_fast
```

Linux 常规构建：

```bash
cd srb_prpr_v8
./build.sh
./submission/bin/estimate -in request.csv -out result.csv
```

也可在 Windows 上用 `build_linux_with_zig.ps1 -ZigExe <zig.exe>` 交叉编译静态 Linux x86-64 程序。编译参数未使用 `-march=native`。

## 当前正式产物（P2 8+8）

- Linux 静态二进制：`submission/bin/estimate`，12,966,280 bytes。
- Windows 本地性能二进制：6,056,448 bytes（`build/` 被 Git 忽略）。
- 8+8 树加三表记忆头：404,160 bytes；其余约 9.3 MB 是无 Atlas 回退参数头。
- 外部 Atlas：0 bytes。

百万请求的五次本地输出 SHA-256 完全一致。详细数据见 `analysis/p2_memory_fast/runtime_benchmark.json` 和 `analysis/p2_memory_fast/p2_cpp_score.json`。公开全量指标已经达线；固定验证仍未达到 94.6，当前定位是“可提交试测、继续优化”。
