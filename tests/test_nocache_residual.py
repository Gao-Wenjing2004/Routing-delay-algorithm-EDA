import csv
import json
import sys
from pathlib import Path

from tools import distill_nocache_residual as residual


def write_panel(root: Path, name: str, offset: int) -> tuple[Path, Path]:
    golden = root / f"{name}_golden.csv"
    trace = root / f"{name}_trace.csv"
    with golden.open("w", newline="", encoding="utf-8") as gf, trace.open(
        "w", newline="", encoding="utf-8"
    ) as tf:
        gw, tw = csv.writer(gf), csv.writer(tf)
        gw.writerow(("From", "To", "Delay"))
        tw.writerow(("From", "To", "Delay", "BaseDelay", "Correction", "Branch",
                     "SourcePort", "TargetPort", "Dx", "Dy", "Gap", "BlockMask"))
        for index in range(96):
            source = f"SRB_{offset}_{index % 96}/ISEA[0]"
            target = f"SRB_{offset + 24}_{index % 96}/ZSEA[0]"
            gw.writerow((source, target, 104))
            tw.writerow((source, target, 100, 100, 0, "short_teacher", 0, 1, 24, 0, 0, 0))
    return golden, trace


def test_fixed_residual_accepts_disjoint_positive_panels(tmp_path, monkeypatch):
    paths = {name: write_panel(tmp_path, name, offset) for name, offset in (
        ("train", 1), ("source", 31), ("region", 61)
    )}
    output = tmp_path / "output"
    arguments = ["distill"]
    for name in ("train", "source", "region"):
        arguments += [f"--{name}-golden", str(paths[name][0]), f"--{name}-trace", str(paths[name][1])]
    arguments += ["--output-dir", str(output)]
    monkeypatch.setattr(sys, "argv", arguments)
    assert residual.main() == 0
    report = json.loads((output / "distillation_report.json").read_text())
    assert report["accepted"] is True
    assert report["request_identity_stored"] is False
    assert all(report["selected"]["gains"][name] > 0 for name in ("train", "source", "region"))
    assert (output / "direct_residual.bin").stat().st_size == 48 + residual.COARSE_SIZE + residual.FINE_SIZE
