#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PRODUCTION_SOURCE_ROOT = (ROOT / "src").resolve()
SOURCE_SUFFIXES = {".cc", ".cpp", ".cxx"}
SHARD_COUNT = 3


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run clang-tidy only for changed translation units."
    )
    parser.add_argument("--base-ref", required=True)
    parser.add_argument("--build-directory", required=True, type=Path)
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--smoke-source", type=Path)
    parser.add_argument(
        "--shard-index", type=int, choices=range(SHARD_COUNT),
        help="Select one of three single-process whole-translation-unit shards.",
    )
    args = parser.parse_args()
    if args.jobs is None:
        args.jobs = 1 if args.shard_index is not None else max(1, min(4, os.cpu_count() or 1))
    elif args.shard_index is not None and args.jobs != 1:
        parser.error("sharded analysis requires --jobs 1")
    return args


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
    commands = {}
    for entry in entries:
        if "output" not in entry:
            continue
        directory = Path(entry["directory"])
        if not directory.is_absolute():
            raise ValueError(f"Compilation directory is not absolute: {directory}")
        source = Path(entry["file"])
        output = Path(entry["output"])
        source = (source if source.is_absolute() else directory / source).resolve()
        output = (output if output.is_absolute() else directory / output).resolve()
        commands[source] = str(output.relative_to(build_root))
    return commands


def is_production_source(path: Path) -> bool:
    try:
        path.resolve().relative_to(PRODUCTION_SOURCE_ROOT)
    except ValueError:
        return False
    return True


def partition_sources(sources: set[Path]) -> list[list[Path]]:
    partitions: list[list[Path]] = [[] for _ in range(SHARD_COUNT)]
    weights = [0] * SHARD_COUNT
    weighted = [(source, max(1, source.stat().st_size)) for source in sources]
    for source, weight in sorted(weighted, key=lambda item: (-item[1], item[0].as_posix())):
        owner = min(range(SHARD_COUNT), key=lambda index: (weights[index], index))
        partitions[owner].append(source)
        weights[owner] += weight
    return [sorted(partition) for partition in partitions]


def main() -> int:
    args = parse_arguments()
    paths = changed_files(args.base_ref)
    database = compile_commands(args.build_directory)
    sources = {
        (ROOT / path).resolve()
        for path in paths
        if path.suffix in SOURCE_SUFFIXES
        and is_production_source(ROOT / path)
        and (ROOT / path).resolve() in database
    }
    if args.smoke_source is not None:
        smoke_source = (ROOT / args.smoke_source).resolve()
        if not is_production_source(smoke_source):
            print(
                f"Smoke source is outside src/: {args.smoke_source}",
                file=sys.stderr,
            )
            return 1
        if smoke_source not in database:
            print(
                f"Smoke source is missing from compile_commands.json: {args.smoke_source}",
                file=sys.stderr,
            )
            return 1
        sources.add(smoke_source)
    assigned = (
        sorted(sources) if args.shard_index is None
        else partition_sources(sources)[args.shard_index]
    )
    targets = sorted(database[source] for source in assigned)
    print("changed-clang-tidy-selection " + json.dumps({
        "schema_version": 1,
        "shard_index": args.shard_index if args.shard_index is not None else 0,
        "shard_count": SHARD_COUNT if args.shard_index is not None else 1,
        "parallel": max(1, args.jobs),
        "selected_sources": [source.relative_to(ROOT).as_posix() for source in sorted(sources)],
        "assigned_sources": [source.relative_to(ROOT).as_posix() for source in assigned],
        "targets": targets,
    }, sort_keys=True), flush=True)
    if not assigned:
        print(
            "No changed translation units require clang-tidy." if not sources
            else "This clang-tidy shard has no assigned translation units."
        )
        return 0

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
