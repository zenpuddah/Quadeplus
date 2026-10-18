"""Inspect a Parquet file or a complete version package independently of C++."""

import argparse
import hashlib
from pathlib import Path
import shlex

import pyarrow.parquet as pq


def inspect_file(path):
    parquet = pq.ParquetFile(path)
    print(f"File: {path}")
    print(f"rows={parquet.metadata.num_rows} row_groups={parquet.metadata.num_row_groups}")
    print("Arrow schema:")
    print(parquet.schema_arrow.remove_metadata())
    print("Dataset metadata:")
    for key, value in sorted((parquet.metadata.metadata or {}).items()):
        if key.startswith(b"quant."):
            print(f"{key.decode()}={value.decode()}")
    print("Physical rows:")
    for row in parquet.read().to_pylist():
        for name, value in row.items():
            if isinstance(value, bytes):
                row[name] = value.hex()
        print(row)


def inspect_package(path, show_evidence):
    marker = shlex.split((path / "COMPLETE").read_text())
    if len(marker) != 4 or marker[0] != "quant-complete-v1":
        raise ValueError("unsupported/incomplete completion marker")
    if "/" in marker[2] or "\\" in marker[2]:
        raise ValueError("unsafe attempt identity")
    attempt = path / f".attempt-{marker[2]}"
    manifest_bytes = (attempt / "manifest.qmanifest").read_bytes()
    if hashlib.sha256(manifest_bytes).hexdigest() != marker[3]:
        raise ValueError("manifest SHA-256 mismatch")
    manifest = shlex.split(manifest_bytes.decode())
    if manifest[0:2] != ["quant-evidence-v1", "quant-observations-v2"]:
        raise ValueError("unsupported evidence/observation encoding")
    if manifest[2:4] != marker[1:3]:
        raise ValueError("version/attempt mismatch")
    print(f"version={manifest[2]} acquisition={manifest[4]} stream_id={manifest[5]}")
    print(f"range=[{manifest[6]},{manifest[7]}) explicit_partial={manifest[8]}")
    count = int(manifest[14])
    if len(manifest) != 15 + count * 5:
        raise ValueError("incomplete evidence inventory")
    consumers = []
    hashes = {}
    parents = []
    for index in range(count):
        name, digest, stage, parent, parent_digest = manifest[15 + index * 5:20 + index * 5]
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("unsafe evidence path")
        file = attempt / relative
        if hashlib.sha256(file.read_bytes()).hexdigest() != digest:
            raise ValueError(f"required file changed/missing: {name}")
        hashes[name] = digest
        parents.append((parent, parent_digest))
        print(f"{stage}: {name} sha256={digest} parent={parent or 'none'}")
        if stage == "consumer":
            consumers.append(file)
    for parent, expected in parents:
        if parent and hashes.get(parent) != expected:
            raise ValueError(f"unresolved stage parent: {parent}")
    print("package_hashes=verified (C++ consumer eligibility checks are separate)")
    if show_evidence:
        for name in ["acquisition.qacq", "references.qref", "validation.qvalidation"]:
            print(f"--- {name} ---")
            print((attempt / name).read_text())
    for file in consumers:
        inspect_file(file)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", help="Parquet file or version directory printed by the manual C++ example")
    parser.add_argument("--evidence", action="store_true", help="also display retained acquisition, reference and validation text")
    args = parser.parse_args()
    path = Path(args.path)
    if path.is_dir():
        inspect_package(path, args.evidence)
    else:
        inspect_file(path)


if __name__ == "__main__":
    main()
