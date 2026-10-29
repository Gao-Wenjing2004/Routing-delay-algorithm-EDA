import json

from tools.analyze_axis_portal_oracle import Architecture, geometry_candidates


def architecture(tmp_path):
    path = tmp_path / "gap.json"
    path.write_text(json.dumps({"Gap": {
        "Line": [
            {"direction": "vertical", "site": 5, "delay": 7},
            {"direction": "horizontal", "site": 9, "delay": 11},
        ],
        "Block": [{
            "lower": 10, "upper": 20, "left": 30, "right": 40,
            "vertical crossable": False, "vertical cross delay": 0,
            "horizontal crossable": True, "horizontal cross delay": 29,
        }],
    }}), encoding="utf-8")
    return Architecture(path)


def test_concrete_segment_gap_and_block_costs(tmp_path):
    arch = architecture(tmp_path)
    assert arch.segment_extra((0, 0), (10, 0)) == 7
    assert arch.segment_extra((35, 0), (35, 30)) is None
    assert arch.segment_extra((20, 15), (50, 15)) == 29
    assert arch.segment_extra((20, 0), (50, 0)) == 0


def test_portal_candidates_use_both_block_sides():
    block = {"left": 30, "right": 40}
    candidates = geometry_candidates((35, 0), (35, 30), [block], 2)
    portal_x = {
        points[1][0] for name, points, _runs in candidates if name == "HVH"
    }
    assert portal_x == {28, 29, 41, 42}

