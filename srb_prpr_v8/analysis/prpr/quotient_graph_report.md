# PRP-R 周期商图报告

本报告由 `tools/build_prpr_assets.py` 仅根据官方 SRB JSON 自动生成；未读取任何 Golden 延时。

## 图构建

- Port：496（Input 208，Output 288）
- Routing Input 周期状态：160
- 周期转移边：2752
- 近似状态类（按 Net 位移）：16，类大小：[16, 16, 16, 16, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8]
- 精确签名等价类：160，最大类大小：[1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]
- 强连通分量：1，分量大小：[160]
- FPGA 尺寸：120 × 550
- Gap：19；Block：7

状态不包含绝对坐标。每条边表示：Routing Input 经一个 Arc 到 Output，再经固定 Net 位移落到下一 Routing Input。Arc delay 计入边权；Net JSON 没有常规 delay 字段，因此常规 Net delay 记为 0，Gap/Block 在后续阶段单独处理。

## Net 位移与跨度

最大 Net 跨度由 JSON 自动得到：**12**。

```text
dx=-10, dy=  0, span=10
dx= -4, dy=  0, span=4
dx= -2, dy=  0, span=2
dx= -1, dy=  0, span=1
dx=  0, dy=-12, span=12
dx=  0, dy= -4, span=4
dx=  0, dy= -2, span=2
dx=  0, dy= -1, span=1
dx=  0, dy=  1, span=1
dx=  0, dy=  2, span=2
dx=  0, dy=  4, span=4
dx=  0, dy= 12, span=12
dx=  1, dy=  0, span=1
dx=  2, dy=  0, span=2
dx=  4, dy=  0, span=4
dx= 10, dy=  0, span=10
```

## 单步单位位移延时下界

- east: 10.300000 ps/unit
- west: 10.400000 ps/unit
- north: 8.666667 ps/unit
- south: 8.750000 ps/unit

这些值只是商图单边下界，不直接当作完整端到端预测；P1 使用自动发现的可重复循环原语。
