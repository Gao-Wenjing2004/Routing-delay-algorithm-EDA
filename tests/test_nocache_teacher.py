import csv
import json
import subprocess
import sys
from pathlib import Path


def endpoint(x: int, y: int, port: str = "A_A0") -> str:
    return f"SRB_{x}_{y}/{port}"


def test_generator_has_disjoint_sources_regions_and_pairs(tmp_path: Path) -> None:
    pool = tmp_path / "pool.csv"
    with pool.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["From", "To"])
        for sx in range(0, 120, 10):
            for sy in range(0, 550, 50):
                source = endpoint(sx, sy)
                for tx in range(0, 120, 6):
                    for ty in range(0, 550, 25):
                        if (sx, sy) != (tx, ty):
                            writer.writerow([source, endpoint(tx, ty, "S_A0")])
    gap = tmp_path / "gap.json"
    gap.write_text(json.dumps({"Gap": {"Line": [
        {"direction": "horizontal", "site": 49},
        {"direction": "vertical", "site": 59},
    ], "Block": [{"left": 70, "right": 90, "lower": 100, "upper": 150}]}}), encoding="utf-8")
    output = tmp_path / "panels"
    subprocess.run([
        sys.executable, "tools/generate_nocache_teacher_requests.py",
        "--pool", str(pool), "--gap", str(gap), "--output-dir", str(output),
        "--per-boundary", "2", "--environment-per-source", "4",
    ], check=True, cwd=Path(__file__).parents[1], capture_output=True, text=True)
    report = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    assert report["leakage"] == {"pair": 0, "source": 0, "region_into_region_holdout": 0}
    pairs: set[tuple[str, str]] = set()
    sources: dict[str, set[str]] = {}
    for split in ("train", "source_holdout", "region_holdout"):
        with (output / f"{split}_requests.csv").open(encoding="utf-8", newline="") as handle:
            rows = list(csv.DictReader(handle))
        sources[split] = {row["From"] for row in rows}
        for row in rows:
            pair = row["From"], row["To"]
            assert pair not in pairs
            pairs.add(pair)
    assert not (sources["train"] & sources["source_holdout"])
    assert not (sources["train"] & sources["region_holdout"])
    assert not (sources["source_holdout"] & sources["region_holdout"])
