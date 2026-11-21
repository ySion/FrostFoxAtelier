"""Paired runs of one diagnostic image, alternating order on one measuring CPU."""

import argparse
import csv
import os
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path("out/field-miss-study"))
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--cpu", type=int, default=18)
    args = parser.parse_args()
    out = args.out.resolve()
    exe = out / "field-study.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path("out/release").resolve()) + os.pathsep + env["PATH"]

    def run(mode):
        return subprocess.check_output([exe, mode, str(args.cpu)], env=env, text=True)

    (out / "catalog.csv").write_text(
        "type,fields,requests,names_over_15_bytes,old_hash_eq,new_hash_eq,old_bang_eq,new_bang_eq\n" + run("catalog"))
    modes = ["tag4", "owned", "fixed", "indexed", "error"]
    samples = {}
    with (out / "samples.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("round", "mode", "metric", "ns"))
        for r in range(args.rounds):
            order = modes[r % len(modes):] + modes[:r % len(modes)]
            if (r // len(modes)) % 2:
                order.reverse()
            for mode in order:
                for row in csv.reader(run(mode).splitlines()):
                    metric, ns = row
                    samples.setdefault(metric, {}).setdefault(mode, []).append(float(ns))
                    writer.writerow((r + 1, mode, metric, ns))
            output.flush()
    with (out / "summary.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("metric", *modes, "owned_minus_tag4_paired_ns", "owned_slower_rounds"))
        for metric, results in samples.items():
            expected = [m for m in modes if m != "error" or metric != "mixed_hit"]
            assert set(results) == set(expected) and all(len(results[m]) == args.rounds for m in expected)
            difference = [b - a for a, b in zip(results["tag4"], results["owned"])]
            writer.writerow((metric, *(round(statistics.median(results[m]), 3) if m in results else "" for m in modes),
                             round(statistics.median(difference), 3), sum(d > 0 for d in difference)))


if __name__ == "__main__":
    main()
