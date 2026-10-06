#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {".cc", ".cpp", ".cxx"}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run clang-tidy only for changed translation units."
    )
    parser.add_argument("--base-ref", required=True)
    parser.add_argument("--build-directory", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=max(1, min(4, os.cpu_count() or 1)))
    parser.add_argument("--smoke-source", type=Path)
    return parser.parse_args()


def changed_files(base_ref: str) -> list[Path]:
    result = subprocess.run(
        [
            "git",
            "diff",
            "--name-only",
            "--diff-filter=ACMR",
            f"{base_ref}...HEAD",
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return [Path(line) for line in result.stdout.splitlines() if line]


def compile_commands(build_directory: Path) -> dict[Path, str]:
    database_path = ROOT / build_directory / "compile_commands.json"
    entries = json.loads(database_path.read_text(encoding="utf-8"))
    build_root = (ROOT / build_directory).resolve()
    return {
        Path(entry["file"]).resolve(): str(
            Path(entry["output"]).resolve().relative_to(build_root)
        )
        for entry in entries
        if "output" in entry
    }


def main() -> int:
    args = parse_arguments()
    paths = changed_files(args.base_ref)
    database = compile_commands(args.build_directory)
    sources = {
        (ROOT / path).resolve()
        for path in paths
        if path.suffix in SOURCE_SUFFIXES and (ROOT / path).resolve() in database
    }
    if args.smoke_source is not None:
        smoke_source = (ROOT / args.smoke_source).resolve()
        if smoke_source not in database:
            print(
                f"Smoke source is missing from compile_commands.json: {args.smoke_source}",
                file=sys.stderr,
            )
            return 1
        sources.add(smoke_source)
    if not sources:
        print("No changed translation units require clang-tidy.")
        return 0

    targets = sorted(database[source] for source in sources)
    print(f"Building {len(targets)} clang-tidy translation unit target(s).")
    completed = subprocess.run(
        [
            "cmake",
            "--build",
            str(ROOT / args.build_directory),
            "--parallel",
            str(max(1, args.jobs)),
            "--target",
            *targets,
        ],
        cwd=ROOT,
        check=False,
    )
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
