# 本地历史版本归档核验

核验日期：2026-10-02。

## 结论

清理前对工作区外的 V3、V4、V5、V6、V7 与仓库归档逐文件进行了 SHA-256 比较。所有具有相同相对路径的源码、模型头、脚本和小型证据均完全一致，未发现哈希不一致。

| 版本 | 已逐文件比对 | 哈希不一致 | Git 中的归档位置 |
|---|---:|---:|---|
| V3 | 22 | 0 | `legacy_versions/v3/` |
| V4 | 15 | 0 | `legacy_versions/v4/` |
| V5 | 25 | 0 | `legacy_versions/v5/` |
| V6 | 8 | 0 | `legacy_versions/v6/` |
| V7 | 28 | 0 | `submission_v7_20260919/` |

V1 没有找到源码。两个本地 `estimate` 完全相同，因此只归档一份到 `legacy_versions/v1_official/bin/estimate`。

## Git LFS 保存的大资产

| 资产 | 字节数 | SHA-256 |
|---|---:|---|
| V1 `bin/estimate` | 1,081,714,760 | `5595150FBEADA22F961EED9468FF2EAD120D69AC3A98FD2EC8BC0845464B9567` |
| V3 `local_atlas.bin` | 653,772,840 | `04FD3FBA2F7AC48A09D7CCCDAB23CF4C7D6A33D313B75AF8CFA85B93A4358750` |
| V7 `bin/estimate` | 1,271,029,532 | `888032D50EA38FB5FCD0BDE4129AF5ECA94128F735643680A7E2120C784E839C` |
| V7 `qa/requests_1m.csv` | 35,394,624 | `B3B7360DCDDF428AC41036BC769D78D4A54D0834A1E29B1C50C1A05595297510` |
| V7 `qa/external_reference_1m.csv` | 40,332,115 | `22BCED0ABBE590B33D20CC4DEF21EB0506D8E2668928D50D09B9C0901F0DDED9` |

## 没有归档的冗余产物

以下文件不定义算法版本，且可由已归档源码、输入或构建脚本重新生成，所以清理时不进入 Git：

- Windows `.exe`、`__pycache__`、解析缓存；
- V3/V5 的百万条预测 CSV；
- V4/V6 的本地性能测试可执行文件；
- `analysis/` 中重复的百万/五百万行 benchmark 输入和预测输出；
- `tmp/` 中的回归副本、临时依赖和渲染缓存。

官方架构 JSON 与 Golden CSV 不在本轮冗余清理范围内。Golden 文件在 `baseline-srb_solver` 和官方资料目录中仍有独立、同哈希副本。

## 恢复方法

完整克隆需要安装 Git LFS：

```bash
git lfs install
git clone https://github.com/Gao-Wenjing2004/Routing-delay-algorithm-EDA.git
cd Routing-delay-algorithm-EDA
git switch v8-prpr
git lfs pull
```

若只研究 V8，可不下载历史大资产，使用 `GIT_LFS_SKIP_SMUDGE=1` 克隆，并直接构建 `srb_prpr_v8/`。

## 清理执行结果

远端 `v8-prpr` 分支确认指向归档提交 `b3aa57095d07deddd867c3b3f8bf4d92bcc670fd` 后，已删除工作区根目录下的六份旧本地副本：

- `sub-thefirsttime/`
- `plusone-srb_fast_v3/`
- `srb_fast_v4/`
- `srb_fast_v5/`
- `srb_fast_v6/`
- `submission_v7_20260919/`

删除后逐项检查均为不存在。当前 Git 仓库、V8、`baseline-srb_solver/`、官方架构/Golden 以及尚未完整归档的分析目录未删除。
