# Routing-delay-algorithm-EDA

面向超大规模 FPGA SRB 阵列的布局布线延时估算算法。评测程序会执行大规模查询，
并综合准确度、运行时间、内存占用和结果一致性评分。

## 当前稳定基线

当前团队基线是 [`submission_v7_20260919`](submission_v7_20260919/)：

- 公开 100 万条准确度得分：`95.9796439241`；
- 固定 20 万留出集准确度得分：`95.6476063175`；
- 单线程 Linux x86-64；
- Atlas 和模型均内嵌，不依赖运行时外部数据；
- 正式二进制由 Git LFS 管理。

V7 的算法说明、验证记录和局限见
[`README_提交说明.md`](submission_v7_20260919/README_提交说明.md)。

## 克隆与准备

所有成员在克隆前安装 Git LFS：

```bash
git lfs install
git clone https://github.com/Gao-Wenjing2004/Routing-delay-algorithm-EDA.git
cd Routing-delay-algorithm-EDA
git lfs pull
```

确认基线二进制已经下载，而不是 LFS 指针：

```bash
git lfs ls-files
ls -lh submission_v7_20260919/bin/estimate
```

## 团队开发约定

- `main` 永远保持当前已验证的最佳版本，不直接在 `main` 上试验。
- 每个优化建立独立分支，例如 `opt/a-20260923-atlas-gating`。
- 先提交源码和评测结果，通过评审后再更新大二进制。
- 没有提升的分支直接关闭，不合并到 `main`。
- 已经合并但后来确认有问题的改动使用 `git revert`，不重写共享历史。

完整操作步骤见 [`docs/TEAM_WORKFLOW.md`](docs/TEAM_WORKFLOW.md)。
