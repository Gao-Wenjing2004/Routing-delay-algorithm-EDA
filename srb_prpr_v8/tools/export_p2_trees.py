#!/usr/bin/env python3
"""Export the selected two-stage LightGBM student to a standalone C++ header."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--teacher", type=Path,
                        default=root / "analysis" / "p2" / "p2_teacher_lightgbm_model.json")
    parser.add_argument("--student", type=Path,
                        default=root / "analysis" / "p2" / "p2_lightgbm_model.json")
    parser.add_argument("--prpr-model", type=Path,
                        default=root / "analysis" / "prpr" / "prpr_model.json")
    parser.add_argument("--post-memory", type=Path,
                        help="optional compact post-tree residual memory JSON")
    parser.add_argument("--output", type=Path,
                        default=root / "p2_runtime" / "p2_student_data.hpp")
    return parser.parse_args()


def cpp_float(value: float) -> str:
    text = format(float(value), ".9g")
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def flatten_model(model: dict[str, object]):
    nodes: list[dict[str, object]] = []
    roots: list[int] = []
    masks: list[tuple[int, int, int, int]] = []

    def add(raw: dict[str, object]) -> int:
        index = len(nodes)
        nodes.append({})
        if "leaf_value" in raw:
            nodes[index] = {
                "feature": -1,
                "left": -1,
                "right": -1,
                "mask": -1,
                "threshold": 0.0,
                "value": float(raw["leaf_value"]),
                "categorical": False,
            }
            return index
        left = add(raw["left_child"])
        right = add(raw["right_child"])
        categorical = str(raw.get("decision_type", "<=")) == "=="
        mask_index = -1
        threshold = 0.0
        if categorical:
            words = [0, 0, 0, 0]
            text = str(raw.get("threshold", ""))
            for item in text.split("||") if text else ():
                category = int(item)
                if not 0 <= category < 256:
                    raise ValueError(f"categorical value outside [0,255]: {category}")
                words[category // 64] |= 1 << (category % 64)
            mask_index = len(masks)
            masks.append(tuple(words))
        else:
            threshold = float(raw["threshold"])
        nodes[index] = {
            "feature": int(raw["split_feature"]),
            "left": left,
            "right": right,
            "mask": mask_index,
            "threshold": threshold,
            "value": 0.0,
            "categorical": categorical,
        }
        return index

    for tree in model["tree_info"]:
        roots.append(add(tree["tree_structure"]))
    return nodes, roots, masks


def endpoint_routes(prpr: dict[str, object]):
    state_class = [int(value) for value in prpr["state_classes"]]
    source_routes: list[int] = []
    target_routes: list[int] = []
    source_classes: list[int] = []
    target_classes: list[int] = []
    for pid in range(len(prpr["port_names"])):
        iid = int(prpr["port_to_input"][pid])
        if iid >= 0:
            source = target = int(prpr["input_to_state"][iid])
        else:
            source = int(prpr["output_nets"][pid][0])
            targets = prpr["target_arcs"][pid]
            target = int(min(targets, key=lambda row: (int(row[1]), int(row[0])))[0]) if targets else -1
        source_routes.append(source if source >= 0 else 160)
        target_routes.append(target if target >= 0 else 160)
        source_classes.append(state_class[source] if source >= 0 else 16)
        target_classes.append(state_class[target] if target >= 0 else 16)
    return source_routes, target_routes, source_classes, target_classes


def array_lines(values, width=16):
    rows = []
    for start in range(0, len(values), width):
        rows.append("    " + ", ".join(str(v) for v in values[start:start + width]) + ",")
    return rows


def float_array_lines(values, width=8):
    rows = []
    for start in range(0, len(values), width):
        rows.append("    " + ", ".join(cpp_float(v) for v in values[start:start + width]) + ",")
    return rows


def int_array_lines(values, width=16):
    rows = []
    for start in range(0, len(values), width):
        rows.append("    " + ", ".join(str(v) for v in values[start:start + width]) + ",")
    return rows


def main() -> int:
    opt = parse_args()
    teacher = json.loads(opt.teacher.read_text(encoding="utf-8"))
    student = json.loads(opt.student.read_text(encoding="utf-8"))
    prpr = json.loads(opt.prpr_model.read_text(encoding="utf-8"))
    if teacher["feature_names"] != student["feature_names"]:
        raise ValueError("teacher/student feature order differs")
    if len(teacher["feature_names"]) != 42:
        raise ValueError("P2 runtime expects exactly 42 features")
    teacher_nodes, teacher_roots, teacher_masks = flatten_model(teacher)
    student_nodes, student_roots, student_masks = flatten_model(student)
    source_routes, target_routes, source_classes, target_classes = endpoint_routes(prpr)

    lines = [
        "// Generated by tools/export_p2_trees.py. Do not edit.",
        "#pragma once",
        "#include <cstdint>",
        "namespace p2_student_data {",
        "struct Node { int16_t feature; int16_t mask; int32_t left; int32_t right; float threshold; float value; uint8_t categorical; };",
        "struct CatMask { uint64_t word[4]; };",
    ]

    def emit_model(prefix, nodes, roots, masks):
        lines.append(f"inline constexpr Node k{prefix}Nodes[{len(nodes)}] = {{")
        for node in nodes:
            lines.append(
                "    {" + ", ".join([
                    str(node["feature"]), str(node["mask"]), str(node["left"]),
                    str(node["right"]), cpp_float(node["threshold"]),
                    cpp_float(node["value"]), "1" if node["categorical"] else "0",
                ]) + "},"
            )
        lines.append("};")
        lines.append(f"inline constexpr int32_t k{prefix}Roots[{len(roots)}] = {{")
        lines.extend(array_lines(roots))
        lines.append("};")
        lines.append(f"inline constexpr CatMask k{prefix}Masks[{max(1, len(masks))}] = {{")
        if masks:
            for words in masks:
                lines.append("    {{" + ", ".join(f"UINT64_C({word})" for word in words) + "}},")
        else:
            lines.append("    {{UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0)}},")
        lines.append("};")
        lines.append(f"inline constexpr int k{prefix}TreeCount = {len(roots)};")

    emit_model("Teacher", teacher_nodes, teacher_roots, teacher_masks)
    emit_model("Student", student_nodes, student_roots, student_masks)

    post_parameters = 0
    if opt.post_memory:
        post = json.loads(opt.post_memory.read_text(encoding="utf-8"))
        expected = [
            "source_family_distance", "macro8_direction", "source_family_macro8",
        ]
        actual = [str(group["name"]) for group in post["groups"]]
        if actual != expected:
            raise ValueError(f"unsupported post-memory groups/order: {actual}")
        post_scale = 1 << 20
        lines.append(f"inline constexpr float kPostAlpha = {cpp_float(post['alpha'])};")
        lines.append(f"inline constexpr float kPostScale = {cpp_float(1.0 / post_scale)};")
        post_quantized = {}
        for group in post["groups"]:
            values = group["values"]
            name = "".join(part.title() for part in str(group["name"]).split("_"))
            quantized = [int(round(float(value) * post_scale)) for value in values]
            if min(quantized) < -32768 or max(quantized) > 32767:
                raise ValueError(f"post-memory group {group['name']} exceeds int16 Q20 range")
            post_quantized[str(group["name"])] = quantized
            lines.append(f"inline constexpr int16_t kPost{name}[{len(values)}] = {{")
            lines.extend(int_array_lines(quantized))
            lines.append("};")
            post_parameters += len(values)
        distance_values = post_quantized["source_family_distance"]
        macro_values = post_quantized["source_family_macro8"]
        family_count = len(macro_values) // 100
        active_families = [
            int(any(distance_values[family * 162:(family + 1) * 162]) or
                any(macro_values[family * 100:(family + 1) * 100]))
            for family in range(family_count)
        ]
        lines.append(f"inline constexpr uint8_t kPostSourceFamilyActive[{family_count}] = {{")
        lines.extend(array_lines(active_families))
        lines.append("};")

    for name, values in (
        ("SourceRoute", source_routes), ("TargetRoute", target_routes),
        ("SourceClass", source_classes), ("TargetClass", target_classes),
    ):
        lines.append(f"inline constexpr uint8_t k{name}[{len(values)}] = {{")
        lines.extend(array_lines(values))
        lines.append("};")

    gaps = list(prpr["gaps"])
    blocks = list(prpr["blocks"])
    lines.extend([
        "struct Gap { uint16_t site; uint16_t delay; uint8_t vertical; };",
        f"inline constexpr Gap kGaps[{len(gaps)}] = {{",
    ])
    for gap in gaps:
        lines.append(f"    {{{int(gap['site'])}, {int(gap['delay'])}, {1 if gap['direction'] == 'vertical' else 0}}},")
    lines.extend([
        "};",
        "struct Block { int16_t lower; int16_t upper; int16_t left; int16_t right; };",
        f"inline constexpr Block kBlocks[{len(blocks)}] = {{",
    ])
    for block in blocks:
        lines.append(
            f"    {{{int(block['lower'])}, {int(block['upper'])}, {int(block['left'])}, {int(block['right'])}}},"
        )
    lines.extend(["};", "}  // namespace p2_student_data", ""])
    opt.output.parent.mkdir(parents=True, exist_ok=True)
    opt.output.write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({
        "output": str(opt.output.resolve()),
        "teacher_trees": len(teacher_roots),
        "student_trees": len(student_roots),
        "teacher_nodes": len(teacher_nodes),
        "student_nodes": len(student_nodes),
        "teacher_masks": len(teacher_masks),
        "student_masks": len(student_masks),
        "post_parameters": post_parameters,
        "post_active_families": sum(active_families) if opt.post_memory else 0,
        "bytes": opt.output.stat().st_size,
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
