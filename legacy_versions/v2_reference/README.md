# V2：局部最短路 Atlas 基线

本机没有发现独立 V2 源码目录。V3 的文档、生成器和结果表共同表明，V2 的核心是“半径 56 的离线 Dijkstra Atlas，查询范围 48”，V3 直接继承它的局部分支。

现存记录：

- 公开 100 万条准确度分：94.218939
- 长距离准确度分：94.225695
- Atlas 复现代码：[`../v3/generate_local_atlas.cpp`](../v3/generate_local_atlas.cpp)
- 架构转换代码：[`../v3/prepare_local_arch.py`](../v3/prepare_local_arch.py)
- Atlas 文件：653,772,840 bytes
- Atlas SHA-256：`04FD3FBA2F7AC48A09D7CCCDAB23CF4C7D6A33D313B75AF8CFA85B93A4358750`

这个版本已证明离线最短路适合作为局部结构 Teacher，但 624 MiB Atlas 不满足现在的 100 MB 提交限制。
