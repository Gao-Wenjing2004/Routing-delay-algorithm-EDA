#!/usr/bin/env python3
"""Encode exact Dijkstra run skeletons as structured-pricer audit candidates."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_\d+_\d+/(.+)$")


def primitive_runs(sequence: str) -> list[list[object]]:
    result: list[list[object]] = []
    for item in sequence.split("|") if sequence else []:
        axis, delta = item.split("@", 1)[0].split(":", 1)
        if result and result[-1][0] == axis:
            result[-1][1] = int(result[-1][1]) + int(delta)
        else:
            result.append([axis, int(delta)])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--only-portal-geometry", action="store_true")
    args = parser.parse_args()

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    port_ids = {name: index for index, name in enumerate(model["port_names"])}
    rows = skipped = 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as source, \
            args.output.open("w", encoding="utf-8", newline="") as target:
        writer = csv.writer(target)
        writer.writerow(("From", "To", "Golden", "Candidates"))
        for raw in csv.DictReader(source):
            if raw["Reachable"] != "1" or not raw["PrimitiveSequence"]:
                skipped += 1
                continue
            if args.only_portal_geometry:
                start = tuple(map(int, raw["From"].split("/", 1)[0].split("_")[1:]))
                finish = tuple(map(int, raw["To"].split("/", 1)[0].split("_")[1:]))
                low_y, high_y = sorted((start[1], finish[1]))
                if not (
                    76 <= start[0] <= 89 and 76 <= finish[0] <= 89 and
                    any(high_y >= lower and low_y <= lower + 49
                        for lower in range(0, 501, 100))
                ):
                    continue
            runs = primitive_runs(raw["PrimitiveSequence"])
            port_match = ENDPOINT.match(raw["From"])
            if port_match is None:
                raise ValueError(f"bad endpoint: {raw['From']}")
            source_port = port_ids[port_match.group(1)]
            iid = int(model["port_to_input"][source_port])
            route = int(model["input_to_state"][iid]) if iid >= 0 else -1
            if route < 0:
                first_delta = int(
                    raw["PrimitiveSequence"].split("|", 1)[0]
                    .split("@", 1)[0].split(":", 1)[1]
                )
                runs[0][1] = int(runs[0][1]) - first_delta
            axes = [str(item[0]) for item in runs]
            values = [int(item[1]) for item in runs]
            h_trunk = max((index for index, axis in enumerate(axes) if axis == "H"), default=-1)
            v_trunk = max((index for index, axis in enumerate(axes) if axis == "V"), default=-1)
            encoded = (
                ">".join(axes) + "@" + str(h_trunk) + ":" + str(v_trunk) + ":" +
                ":".join(map(str, values)) + "S"
            )
            writer.writerow((raw["From"], raw["To"], raw["Delay"], encoded))
            rows += 1
    print(json.dumps({"rows": rows, "skipped": skipped}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
