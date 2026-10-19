# K48 已提交版本（2026-10-15）

本目录保存用户确认已提交比赛平台的原版 **K48-Lazy**，来源为
`submission_k48_20261015/submission.zip`。源码、程序、七个运行资产和压缩包均保持原字节。

| 平台指标 | 用户报告值 |
|---|---:|
| 准确度得分 | 97.42 |
| 时间得分 | 85.82 |
| 显示运行耗时 | 250887 ms（250.887 s） |
| 总分 | 95.81 |

以上是用户报告的平台成绩，未在本次上传时重新测量。历史内部验证分数属于不同数据和测量，
不与这些平台成绩混用。本目录不包含后续 RAM、HGB、floor 或 R5 候选。

## 获取本版本

本版本的程序、资产、原 ZIP 及较大的生成模型头文件使用 Git LFS。
安装 Git LFS 后，可仅取本版本所需对象，避免下载 V7 的大程序：

```sh
git lfs install
GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --single-branch --branch wufan https://github.com/Gao-Wenjing2004/Routing-delay-algorithm-EDA.git
cd Routing-delay-algorithm-EDA
git lfs pull --include="submission_k48_20261015/**"
cd submission_k48_20261015
python3 verify_release.py
```

校验前须确认 LFS 下载已完成，文件内容不能是短小的 LFS 指针文本。

## 直接提交比赛

上传本目录的 **`submission.zip`**，不要重新压缩整个源码目录。

- 压缩包大小：91,242,296 字节，小于 100,000,000 字节。
- SHA256：`859765a3288742cf282b42c68948c7204d86f87e724e23a1d5c0d2b3b4420003`。
- 解压后只有 `bin/estimate` 及 `bin/assets/` 下七个资产，共 97,989,218 字节。
- ZIP 中程序权限为 0755，资产权限为 0644。

## 运行与构建

在 Linux x86-64 环境，从本目录执行：

```sh
chmod +x bin/estimate
./bin/estimate -in request.csv -out result.csv
```

保持程序与 `assets/` 的相对位置。输入列为 `From,To`，输出列为 `From,To,Delay`，单线程运行。

原程序使用项目 Ubuntu 22.04 / GCC 11.4 环境构建，C++17，目标为通用 x86-64。
zlib、liblzma、libstdc++、libgcc 静态链接，仍需要兼容的 Linux libc 加载器。
源码重建需要 GCC/G++、Python 3、strip，以及 zlib/liblzma 的静态开发库：

```sh
sh build.sh
```

`build.sh` 为原冻结脚本。重建会更新 `bin/estimate` 和生成构建报告；不同工具链可能生成不同程序字节。
保留的 `submission.zip` 是原始比赛包，重建不会自动替换它。

## 完整性检查

安装 Python 3 后执行：

```sh
python3 verify_release.py
```

检查包括 65 个冻结源码、构建脚本、8 个运行文件、原始 ZIP 的 SHA256，
以及 ZIP 成员、CRC、成员 SHA256 和 Unix 权限。这不是重新训练或重测预测精度。

`qa/release_manifest.json` 为本次公开清单，记录相对路径、文件身份和用户报告成绩。
`qa/original_*.json` 是原构建时的历史记录；其中待平台验证等状态反映当时进度，
最新成绩及版本确认以本说明和公开清单为准。

## 方法简述

K48 使用端口状态与源端强制 Net 归一化，结合 R24 Local 表、Short/Long 路径表、
Pair30 目标代表映射，以及 K48 Short 扩展和已冻结的残差模型。
查询时查表并进行有限的模型修正；运行资产均包含在本目录中，不需要训练数据或外部服务器。

本次上传只归档已得分版本，没有修改算法或模型参数。
