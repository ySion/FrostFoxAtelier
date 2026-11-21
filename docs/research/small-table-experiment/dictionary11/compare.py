"""Run the two native backends and .NET on one validated little-endian trace."""

import argparse
import csv
import hashlib
import itertools
import os
from pathlib import Path
import random
import statistics
import struct
import subprocess


def u32(value):
    return struct.pack("<I", value)


def text(value):
    data = value.encode("ascii")
    return u32(len(data)) + data


def mixed(key):
    mask = (1 << 64) - 1
    key = (key + 0x9E3779B97F4A7C15) & mask
    key = ((key ^ (key >> 30)) * 0xBF58476D1CE4E5B9) & mask
    key = ((key ^ (key >> 27)) * 0x94D049BB133111EB) & mask
    return key ^ (key >> 31)


def integer_cases(sizes):
    for size in sizes:
        for label, encode in (("dense", int), ("spread", mixed)):
            entries = b"".join(struct.pack("<Q", encode(i)) for i in range(size))
            for mode in ("hit", "miss", "half"):
                rng = random.Random(1203 + size)
                count = 32768
                present = [mode == "hit" or (mode == "half" and i % 2 == 0) for i in range(count)]
                rng.shuffle(present)
                queries = bytearray()
                for hit in present:
                    i = rng.randrange(size)
                    key = encode(i if hit else size + i)
                    queries.extend(struct.pack("<QI", key, i + 1 if hit else 0))
                yield f"{label}_{size}_{mode}", u32(0) + u32(size) + entries + u32(count) + queries


def name_cases(members):
    for kind in ("field", "method"):
        by_type = {}
        for row in members.splitlines():
            member_kind, t, name = row.split("\t")
            if member_kind == kind:
                by_type.setdefault(int(t), []).append(name)
        tables = [names for names in by_type.values() if len(names) <= 4]
        assert tables and all(len(set(table)) == len(table) for table in tables)
        candidates = [(t, i, key) for t, table in enumerate(tables) for i, key in enumerate(table)]
        entries = b"".join(u32(len(table)) + b"".join(map(text, table)) for table in tables)
        print(f"{kind}: {len(tables)} nonempty small tables, {len(candidates)} keys", flush=True)
        for mode in ("hit", "miss"):
            rng = random.Random(1203)
            queries = bytearray()
            for _ in range(4096):
                t, i, key = rng.choice(candidates)
                if mode == "miss":
                    key = key[:-1] + "#"
                    assert key not in tables[t]
                queries.extend(u32(t) + text(key) + u32(i + 1 if mode == "hit" else 0))
            yield f"{kind}_small_{mode}", u32(1) + u32(len(tables)) + entries + u32(4096) + queries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path("out/dictionary11"))
    parser.add_argument("--cpu", type=int, default=18)
    parser.add_argument("--rounds", type=int, choices=range(1, 101), default=12, metavar="N")
    parser.add_argument("--capacity-control", action="store_true",
                        help="Compare two .NET reservations on the 65,536-key traces")
    args = parser.parse_args()
    directory = args.out.resolve()
    native = directory / "dictionary-lookup.exe"
    managed = directory / "managed/Lookup.dll"
    env = dict(os.environ)
    env["PATH"] = str(Path("out/release").resolve()) + os.pathsep + env["PATH"]
    dotnet = ["dotnet", "exec", "--fx-version", "11.0.0", managed]
    if args.capacity_control:
        cases = dict(integer_cases((65536,)))
        prefix = "capacity-"
        runners = {"default": dotnet, "matched": dotnet}
        reference, candidate = "default", "matched"
    else:
        members = subprocess.check_output([native, "members"], env=env, text=True)
        cases = dict(itertools.chain(integer_cases((4, 1024, 65536)), name_cases(members)))
        prefix = ""
        runners = {"ankerl": [native, "ankerl"], "atelier": [native, "atelier"], "dotnet": dotnet}
        reference, candidate = "atelier", "dotnet"
    trace = directory / (prefix + "lookup.bin")
    trace.write_bytes(u32(len(cases)) + b"".join(text(name) + body for name, body in cases.items()))
    print(f"trace SHA-256 {hashlib.sha256(trace.read_bytes()).hexdigest()}", flush=True)

    orders = list(itertools.permutations(runners))
    samples = {name: {impl: [] for impl in runners} for name in cases}
    checksums = {}
    with (directory / (prefix + "samples.csv")).open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("round", "implementation", "metric", "ns", "checksum", "gc0"))
        for r in range(args.rounds):
            for impl in orders[r % len(orders)]:
                command = [*runners[impl], trace, str(args.cpu)]
                if args.capacity_control:
                    # The nearest supported prime to ankerl's 131,072 slots is 130,363.
                    command.append("130363" if impl == "matched" else "65536")
                lines = subprocess.check_output(command, env=env, text=True)
                seen = set()
                for line in lines.splitlines():
                    name, ns, checksum, gc0 = line.split(",")
                    if name in seen or checksums.setdefault(name, checksum) != checksum:
                        raise ValueError(line)
                    seen.add(name)
                    samples[name][impl].append(float(ns))
                    writer.writerow((r + 1, impl, name, ns, checksum, gc0))
                if seen != cases.keys():
                    raise ValueError(f"Incomplete output from {impl}")
                output.flush()
            print(f"round {r + 1}/{args.rounds}", flush=True)

    with (directory / (prefix + "summary.csv")).open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("metric", *runners, f"{candidate}_vs_{reference}_paired_pct", f"{candidate}_faster_rounds"))
        for name, backends in samples.items():
            deltas = [100 * (d / a - 1) for d, a in zip(backends[candidate], backends[reference])]
            writer.writerow((name, *(f"{statistics.median(v):.3f}" for v in backends.values()),
                             f"{statistics.median(deltas):.2f}", sum(d < 0 for d in deltas)))


if __name__ == "__main__":
    main()
