# 团队协作流程

## 一、基本原则

`main` 只保存经过验证的最佳版本。每个人的实验都从最新 `main` 建立独立分支，
通过 Pull Request 比较指标后再决定是否合并。

不要在共享分支上使用 `git push --force`，也不要用 `reset --hard` 回退已经推送的
团队历史。正式回退使用 `git revert`，这样每次变化都可审计、可恢复。

V7 的 1.27 GB 二进制由 Git LFS 管理。每次修改并推送它都会产生一个完整的新
LFS 对象，所以实验阶段只提交源码与指标；确认成为新最佳版本后，才更新正式二进制。

## 二、首次加入项目

仓库所有者先在 GitHub 仓库页面执行：

1. `Settings -> Collaborators`，邀请组员的 GitHub 账号。
2. `Settings -> Branches` 或 `Rules -> Rulesets`，保护 `main`。
3. 要求通过 Pull Request 合并，并至少需要一名组员批准。
4. 禁止 force push 和删除 `main`。

每位组员本地执行：

```bash
git lfs install
git clone https://github.com/Gao-Wenjing2004/Routing-delay-algorithm-EDA.git
cd Routing-delay-algorithm-EDA
git lfs pull
git config user.name "你的名字"
git config user.email "你的邮箱"
```

## 三、组员 A 开始优化

先同步稳定基线，再创建分支：

```bash
git switch main
git pull --ff-only origin main
git switch -c opt/a-20260923-your-topic
```

修改源码并运行验证。实验结果建议保存在：

```text
experiments/a-20260923-your-topic/metrics.json
experiments/a-20260923-your-topic/README.md
```

至少记录：

- 基线和候选版本的准确度；
- 固定留出集指标，不能只写训练集结果；
- 运行时间、峰值内存、输出哈希；
- 使用的数据切分和命令；
- 是否改变正式二进制。

提交并推送自己的分支：

```bash
git status
git diff --check
git add src experiments docs
git commit -m "experiment: describe the optimization"
git push -u origin opt/a-20260923-your-topic
```

然后在 GitHub 创建 Pull Request，目标分支选择 `main`。

如果结果更好，经过复核后生成正式二进制并提交。注意它由 LFS 管理：

```bash
git add submission_v7_20260919/bin/estimate
git commit -m "release: update accepted estimator binary"
git push
```

如果没有提升，不要合并：关闭 Pull Request，保留分支作为实验记录，或删除远端分支：

```bash
git push origin --delete opt/a-20260923-your-topic
```

删除远端分支前，确认其中没有仍需保留的实验结论。

## 四、第二天组员 B 接手

B 不应在 A 的工作目录或未完成分支上直接继续，而应重新从最新稳定版开始：

```bash
git switch main
git pull --ff-only origin main
git switch -c opt/b-20260924-your-topic
```

如果确实需要基于 A 尚未合并的成果，可以显式拉取并建立自己的分支：

```bash
git fetch origin
git switch -c opt/b-20260924-follow-a origin/opt/a-20260923-your-topic
```

Pull Request 中必须写清楚它依赖 A 的哪个分支或提交。

## 五、如何安全回退

### 尚未提交的单个文件

先查看差异，再恢复：

```bash
git diff -- path/to/file
git restore -- path/to/file
```

### 本地提交尚未推送

推荐先创建备份分支，再回到 `main`：

```bash
git branch backup/my-experiment
git switch main
git pull --ff-only origin main
```

### 已推送或已合并到 main

不要重写历史。找到引入问题的提交并创建反向提交：

```bash
git log --oneline --decorate
git switch main
git pull --ff-only origin main
git switch -c revert/bad-change
git revert <bad_commit_sha>
git push -u origin revert/bad-change
```

随后通过 Pull Request 把回退提交合并到 `main`。

如果要回退一个 merge commit：

```bash
git revert -m 1 <merge_commit_sha>
```

### 回到某个已验证版本做新实验

不要移动 `main`，从标签或提交新建分支：

```bash
git switch -c experiment/from-v7 baseline-v7-20260919
```

## 六、发布新的最佳版本

合并通过后，在最新 `main` 上建立不可变标签：

```bash
git switch main
git pull --ff-only origin main
git tag -a baseline-v8-YYYYMMDD -m "validated best baseline v8"
git push origin baseline-v8-YYYYMMDD
```

建议为每个正式版本新建目录，例如 `submission_v8_YYYYMMDD/`，不要直接覆盖V7源码和
验证报告。这样可以随时复现和比较历史最佳版本。

## 七、V7重建说明

当前提交保存了正式二进制和源码快照，但未把可再生成的百万行CSV放入Git。
构建脚本需要 `endpoint_packed.bin`；可从正式二进制提取：

```bash
python tools/extract_v7_atlas.py \
  submission_v7_20260919/bin/estimate \
  endpoint_packed.bin

SRB_ATLAS_FILE="$PWD/endpoint_packed.bin" \
  sh submission_v7_20260919/build.sh
```

`endpoint_packed.bin` 已被 `.gitignore` 忽略，不要重复提交。只有最终验收通过的
`bin/estimate` 才应进入 Git LFS。
