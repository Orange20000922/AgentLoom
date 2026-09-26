#!/usr/bin/env python3
"""Extract the payload of a .conda package into a deterministic directory."""

from __future__ import annotations

import argparse
import shutil
import tarfile
import zipfile
from pathlib import Path

import zstandard


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    archive = args.archive.resolve()
    output = args.output.resolve()
    work = output.parent / f".{output.name}.conda-extract"

    if not archive.is_file() or not zipfile.is_zipfile(archive):
        raise SystemExit(f"invalid .conda archive: {archive}")

    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(output, ignore_errors=True)
    work.mkdir(parents=True)
    output.mkdir(parents=True)

    try:
        with zipfile.ZipFile(archive) as package:
            payload_names = [
                name
                for name in package.namelist()
                if name.startswith("pkg-") and name.endswith(".tar.zst")
            ]
            if len(payload_names) != 1:
                raise RuntimeError(
                    f"expected one pkg-*.tar.zst payload, found {len(payload_names)}"
                )
            package.extract(payload_names[0], work)

        payload = work / payload_names[0]
        tar_path = work / "payload.tar"
        with payload.open("rb") as source, tar_path.open("wb") as destination:
            zstandard.ZstdDecompressor().copy_stream(source, destination)

        with tarfile.open(tar_path) as package:
            package.extractall(output, filter="data")
    except Exception:
        shutil.rmtree(output, ignore_errors=True)
        raise
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"extracted {archive} to {output}")


if __name__ == "__main__":
    main()
