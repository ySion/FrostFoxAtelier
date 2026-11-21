"""Measure each hash on the same registered names and imported vertex bytes."""

import csv
import os
from pathlib import Path
import statistics
import subprocess


def main():
    out = Path("out/foundation-hash")
    mesh = Path("examples/Content/Procedural/Meshes/TerrainGrid.obj")
    env = dict(os.environ)
    env["PATH"] = str(Path("out/release").resolve()) + os.pathsep + env["PATH"]
    modes = [("before", "ankerl"), ("before", "wy"), ("before", "xxh3"),
             ("after", "project"), ("before", "weld"), ("after", "weld")]
    samples = {}
    signature = None
    with (out / "samples.csv").open("w", newline="") as raw:
        writer = csv.writer(raw)
        writer.writerow(("round", "binary", "mode", "metric", "value"))
        for r in range(24):
            order = modes[r % len(modes):] + modes[:r % len(modes)]
            if (r // len(modes)) % 2:
                order.reverse()
            for binary, mode in order:
                result = subprocess.check_output([out / (binary + ".exe"), mesh, out / "members.tsv", mode],
                                                 env=env, text=True)
                for metric, value in csv.reader(result.splitlines()):
                    writer.writerow((r + 1, binary, mode, metric, value))
                    if metric == "weld_digest":
                        if signature is not None and value != signature:
                            raise ValueError("Welded vertices, indices or faces differ.")
                        signature = value
                    else:
                        samples.setdefault((binary, mode, metric), []).append(float(value))
    with (out / "summary.csv").open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(("binary", "mode", "metric", "median_ns", "paired_percent_vs_before", "faster_rounds"))
        for (binary, mode, metric), values in samples.items():
            ref = samples["before", "weld" if mode == "weld" else "ankerl", metric]
            assert len(ref) == len(values) == 24
            diff = [100 * (x / y - 1) for x, y in zip(values, ref)]
            writer.writerow((binary, mode, metric, round(statistics.median(values), 4),
                             round(statistics.median(diff), 2), sum(d < 0 for d in diff)))
    print("weld digest", signature)


if __name__ == "__main__":
    main()
