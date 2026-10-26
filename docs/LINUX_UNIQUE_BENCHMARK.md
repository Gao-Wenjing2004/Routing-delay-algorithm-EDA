# Linux 唯一请求正式参考门禁

`benchmark/linux-unique-10m-100m` 分支只用于性能证据，不是新的算法提交分支。
流水线在同一个 Ubuntu 22.04 runner 上依次生成并测试 1000 万、5000 万、1 亿条请求；
三档使用互斥的 source fold，每档内部 `(From, To)` 唯一，并经 1024 个外存桶打散。

每档运行正式 `bin/estimate_v67` ELF 五次，逐轮核验输出行数和 SHA-256，预测文件校验后立即删除。
10M 第一轮先删除精确的 `/tmp/srb_runtime_atlas_v67.bin`，用于测得一次性冷启动开销；其余轮次估计稳态斜率。
十亿条参考时间为“固定项 + 每行斜率 × 10 亿 + 一次冷启动开销”，不会漏算 Atlas 构建，也不会重复算三次。

只有同时满足以下条件，`reference_quality` 才能由 `pilot` 变为 `release-reference`：

- Linux 主机运行 ELF；
- 10M、50M、100M 三个唯一规模；
- 每档至少五轮且所有输出哈希一致；
- 三档拟合成功；
- 生成清单证明行数、零重复构造不变量及打散程度。

最终小型证据写回 `analysis/unique_eval/`；数 GB 输入和预测不会进入 Git。
该结果是固定 GitHub runner 上的可复现 Linux 工程参考，不等同于官方隐藏机器实测分数。
