"""Extract the supplied Synty Unity package as data; never import its C# code."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import tarfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("unity_project", type=Path)
    args = parser.parse_args()
    root = args.unity_project.resolve()
    with tarfile.open(args.package, "r:gz") as archive:
        members = {m.name: m for m in archive if m.isfile()}
        paths = {}
        for name, member in members.items():
            if name.endswith("/pathname"):
                # This package's pathname records include a trailing '\n00'.
                path = archive.extractfile(member).read().decode("utf-8-sig").splitlines()[0]
                relative = PurePosixPath(path)
                if (relative.is_absolute() or ".." in relative.parts or not relative.parts or
                        relative.parts[0] != "Assets" or "\\" in path or ":" in path):
                    raise ValueError(f"Unsafe package path: {path}")
                paths[name.split("/")[0]] = path
        scripts = {guid for guid, path in paths.items() if Path(path).suffix.lower() in
                   {".cs", ".js", ".boo", ".dll", ".asmdef", ".asmref"}}
        records = []
        for guid, path in paths.items():
            if guid in scripts:
                continue
            target = root / path
            member = members.get(guid + "/asset")
            record = {"guid": guid, "path": path}
            if member:
                data = archive.extractfile(member).read()
                record["source_sha256"] = hashlib.sha256(data).hexdigest()
                # Remove references/components of the removed package scripts.
                if data.startswith(b"%YAML"):
                    text = data.decode("utf-8-sig")
                    chunks = re.split(r"(?=^--- !u!)", text, flags=re.M)
                    removed = []
                    kept = []
                    for chunk in chunks:
                        script = re.search(r"m_Script:.*guid: ([0-9a-f]+)", chunk)
                        if chunk.startswith("--- !u!114 ") and script and script[1] in scripts:
                            removed.append(re.search(r"&(-?\d+)", chunk)[1])
                        else:
                            kept.append(chunk)
                    text = "".join(kept)
                    for file_id in removed:
                        text = re.sub(r"^  - component: \{fileID: " + file_id + r"\}\n", "", text, flags=re.M)
                    data = text.encode("utf-8")
                    if removed:
                        record["removed_script_components"] = len(removed)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                record["import_sha256"] = hashlib.sha256(data).hexdigest()
                record["bytes"] = len(data)
            else:
                target.mkdir(parents=True, exist_ok=True)
            meta = members.get(guid + "/asset.meta")
            if meta:
                Path(str(target) + ".meta").write_bytes(archive.extractfile(meta).read())
            records.append(record)
    manifest = {"package": args.package.name,
                "sha256": hashlib.file_digest(args.package.open("rb"), "sha256").hexdigest(),
                "removed_scripts": [paths[guid] for guid in sorted(scripts)], "assets": records}
    (root.parent / "source-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"Extracted {len(records)} records; excluded {len(scripts)} scripts.")


if __name__ == "__main__":
    main()
