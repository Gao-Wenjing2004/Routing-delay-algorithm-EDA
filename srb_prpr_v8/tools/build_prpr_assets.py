#!/usr/bin/env python3
"""Build the V8 PRP-R quotient graph and architecture-only primitive library."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import asdict
import json
import math
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from prpr_arch import (  # noqa: E402
    DIRECTION_BINS,
    build_class_edges,
    discover_class_connectors,
    direction_name,
    discover_primitives,
    load_architecture,
    primitive_rows,
    select_basis,
    select_runtime_primitives,
    write_csv,
)


def parse_args() -> argparse.Namespace:
    root = HERE.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=root / "analysis" / "prpr")
    parser.add_argument("--max-depth", type=int, default=16)
    parser.add_argument("--max-displacement", type=int, default=128)
    parser.add_argument("--layer-cap", type=int, default=24000)
    parser.add_argument("--primitive-limit", type=int, default=512)
    return parser.parse_args()


def main() -> int:
    opt = parse_args()
    output_dir = opt.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    arch = load_architecture(opt.arch_dir.resolve())
    primitives, class_edges = discover_primitives(
        arch,
        max_depth=opt.max_depth,
        max_displacement=opt.max_displacement,
        layer_cap=opt.layer_cap,
        global_limit=opt.primitive_limit,
    )
    basis = select_basis(primitives)
    runtime_primitives = select_runtime_primitives(primitives)
    class_connectors = discover_class_connectors(
        class_edges, arch.approximate_class_count
    )

    input_pid_by_state = [arch.input_ports[iid] for iid in arch.state_to_input]
    transition_rows = []
    for edge_id, edge in enumerate(arch.edges):
        transition_rows.append(
            {
                "edge_id": edge_id,
                "from_state": edge.from_state,
                "to_state": edge.to_state,
                "from_port": arch.port_names[edge.from_input_port],
                "via_output_port": arch.port_names[edge.via_output_port],
                "to_port": arch.port_names[input_pid_by_state[edge.to_state]],
                "delta_x": edge.dx,
                "delta_y": edge.dy,
                "arc_delay": edge.cost,
                "net_delay": 0,
                "total_delay": edge.cost,
                "net_family": arch.port_names[edge.via_output_port].split("[")[0],
                "net_span": max(abs(edge.dx), abs(edge.dy)),
                "direction": direction_name(edge.dx, edge.dy),
            }
        )
    write_csv(
        output_dir / "state_transitions.csv",
        transition_rows,
        list(transition_rows[0]),
    )

    equivalence_rows = []
    for state in range(arch.state_count):
        input_pid = input_pid_by_state[state]
        equivalence_rows.append(
            {
                "state": state,
                "routing_input": arch.port_names[input_pid],
                "incoming_output": arch.port_names[arch.state_incoming_output[state]],
                "incoming_dx": arch.state_incoming_dx[state],
                "incoming_dy": arch.state_incoming_dy[state],
                "approximate_class": arch.approximate_classes[state],
                "exact_signature_class": arch.exact_classes[state],
                "scc_id": arch.scc_ids[state],
            }
        )
    write_csv(
        output_dir / "state_equivalence.csv",
        equivalence_rows,
        list(equivalence_rows[0]),
    )

    rows = primitive_rows(primitives)
    write_csv(output_dir / "routing_primitives.csv", rows, list(rows[0]))

    spans = sorted(
        {
            (edge.dx, edge.dy, max(abs(edge.dx), abs(edge.dy)))
            for edge in arch.edges
        }
    )
    scc_sizes = sorted(Counter(arch.scc_ids).values(), reverse=True)
    approximate_sizes = sorted(Counter(arch.approximate_classes).values(), reverse=True)
    exact_sizes = sorted(Counter(arch.exact_classes).values(), reverse=True)
    unit_lower: dict[str, float] = {}
    for label, predicate in (
        ("east", lambda dx, dy: dx > 0 and dy == 0),
        ("west", lambda dx, dy: dx < 0 and dy == 0),
        ("north", lambda dx, dy: dy > 0 and dx == 0),
        ("south", lambda dx, dy: dy < 0 and dx == 0),
    ):
        values = [
            edge.cost / max(abs(edge.dx) + abs(edge.dy), 1)
            for edge in arch.edges
            if predicate(edge.dx, edge.dy)
        ]
        unit_lower[label] = min(values) if values else math.inf

    report = f"""# PRP-R 周期商图报告

本报告由 `tools/build_prpr_assets.py` 仅根据官方 SRB JSON 自动生成；未读取任何 Golden 延时。

## 图构建

- Port：{len(arch.port_names)}（Input {len(arch.input_ports)}，Output {len(arch.port_names) - len(arch.input_ports)}）
- Routing Input 周期状态：{arch.state_count}
- 周期转移边：{len(arch.edges)}
- 近似状态类（按 Net 位移）：{arch.approximate_class_count}，类大小：{approximate_sizes}
- 精确签名等价类：{arch.exact_class_count}，最大类大小：{exact_sizes[:16]}
- 强连通分量：{len(scc_sizes)}，分量大小：{scc_sizes}
- FPGA 尺寸：{arch.width} × {arch.height}
- Gap：{len(arch.gaps)}；Block：{len(arch.blocks)}

状态不包含绝对坐标。每条边表示：Routing Input 经一个 Arc 到 Output，再经固定 Net 位移落到下一 Routing Input。Arc delay 计入边权；Net JSON 没有常规 delay 字段，因此常规 Net delay 记为 0，Gap/Block 在后续阶段单独处理。

## Net 位移与跨度

最大 Net 跨度由 JSON 自动得到：**{max(span for _, _, span in spans)}**。

```text
{chr(10).join(f'dx={dx:>3}, dy={dy:>3}, span={span}' for dx, dy, span in spans)}
```

## 单步单位位移延时下界

{chr(10).join(f'- {name}: {value:.6f} ps/unit' for name, value in unit_lower.items())}

这些值只是商图单边下界，不直接当作完整端到端预测；P1 使用自动发现的可重复循环原语。
"""
    (output_dir / "quotient_graph_report.md").write_text(report, encoding="utf-8")

    direction_counts = Counter(primitive.direction_bin for primitive in primitives)
    class_counts = Counter(primitive.entry_class for primitive in primitives)
    scale_counts = Counter(
        "0-16"
        if primitive.displacement <= 16
        else "17-32"
        if primitive.displacement <= 32
        else "33-64"
        if primitive.displacement <= 64
        else "65-96"
        if primitive.displacement <= 96
        else "97+"
        for primitive in primitives
    )
    basis_lines = [
        f"- bin {primitive.direction_bin}: id={primitive.primitive_id}, "
        f"d=({primitive.dx},{primitive.dy}), cost={primitive.total_delay}, "
        f"edges={primitive.number_of_edges}, cost/unit={primitive.delay_per_unit:.6f}"
        for primitive in basis
    ]
    primitive_report = f"""# PRP-R 路由原语覆盖报告

## 离线发现结果

- 搜索图：{arch.approximate_class_count} 个近似周期状态类、{len(class_edges)} 条去重类转移。
- 受限路径深度：1～{opt.max_depth}。
- 位移边界：每轴 ±{opt.max_displacement}。
- 筛选后的可重复原语：{len(primitives)}（上限 {opt.primitive_limit}）。
- 方向桶：{DIRECTION_BINS}；每桶数量：{dict(sorted(direction_counts.items()))}。
- 尺度分布：{dict(sorted(scale_counts.items()))}。
- 可进入原语的状态类：{len(class_counts)}/{arch.approximate_class_count}；每类数量：{dict(sorted(class_counts.items()))}。

原语要求返回同一近似状态类，因此可以整数重复；类内具体 lane 差异作为 P1 误差来源保留，不伪称 Exact。

## 每个方向桶的结构基础原语

{chr(10).join(basis_lines)}

## 公开请求覆盖

公开请求的“双原语＋余数”覆盖率由 `training/evaluate_first_stage.py` 写入最终报告；本文件当前只记录架构侧覆盖。P1 不读取 Atlas，也不保存 `(portA, portB, dx, dy)` 稠密表。
"""
    (output_dir / "primitive_coverage_report.md").write_text(
        primitive_report, encoding="utf-8"
    )

    model = {
        "schema": "prpr-architecture-v1",
        "source": "official SRB JSON only; no Golden delay",
        "width": arch.width,
        "height": arch.height,
        "port_names": arch.port_names,
        "port_directions": arch.port_directions,
        "port_to_input": arch.port_to_input,
        "input_ports": arch.input_ports,
        "input_to_state": arch.input_to_state,
        "state_to_input": arch.state_to_input,
        "state_classes": arch.approximate_classes,
        "state_incoming_dx": arch.state_incoming_dx,
        "state_incoming_dy": arch.state_incoming_dy,
        "output_nets": arch.output_nets,
        "direct_arcs": arch.direct_arcs,
        "target_arcs": arch.target_arcs,
        "class_connectors": class_connectors,
        "gaps": arch.gaps,
        "blocks": arch.blocks,
        "basis": [asdict(primitive) for primitive in basis],
        "runtime_primitives": [asdict(primitive) for primitive in runtime_primitives],
        "primitives": [asdict(primitive) for primitive in primitives],
        "primitive_count": len(primitives),
        "unit_lower_bounds": unit_lower,
    }
    (output_dir / "prpr_model.json").write_text(
        json.dumps(model, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(
        json.dumps(
            {
                "states": arch.state_count,
                "edges": len(arch.edges),
                "approximate_classes": arch.approximate_class_count,
                "exact_classes": arch.exact_class_count,
                "sccs": len(scc_sizes),
                "primitives": len(primitives),
                "basis": len(basis),
                "runtime_primitives": len(runtime_primitives),
                "output_dir": str(output_dir),
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
