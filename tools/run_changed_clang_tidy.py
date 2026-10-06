#!/usr/bin/env python3

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
FULL_CHECK_SUFFIXES = {".h", ".hpp", ".hh", ".hxx"}
FULL_CHECK_NAMES = {"CMakeLists.txt", ".clang-tidy"}
FULL_CHECK_PREFIXES = ("cmake/",)
SOURCE_SUFFIXES = {".cc", ".cpp", ".cxx"}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run clang-tidy only for changed translation units."
    )
    parser.add_argument("--base-ref", required=True)
    parser.add_argument("--build-directory", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=max(1, min(4, os.cpu_count() or 1)))
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


def requires_full_check(paths: list[Path]) -> bool:
    for path in paths:
        text = path.as_posix()
        if path.suffix in FULL_CHECK_SUFFIXES:
            return True
        if path.name in FULL_CHECK_NAMES:
            return True
        if text.startswith(FULL_CHECK_PREFIXES):
            return True
    return False


def compile_commands(build_directory: Path) -> dict[Path, dict[str, object]]:
    database_path = ROOT / build_directory / "compile_commands.json"
    entries = json.loads(database_path.read_text(encoding="utf-8"))
    return {Path(entry["file"]).resolve(): entry for entry in entries}


def run_clang_tidy(path: Path, build_directory: Path) -> tuple[Path, int]:
    command = [
        "clang-tidy",
        f"--config-file={ROOT / '.clang-tidy'}",
        "--warnings-as-errors=*",
        "--extra-arg-before=--driver-mode=g++",
        "-p",
        str(ROOT / build_directory),
        str(path),
    ]
    completed = subprocess.run(command, cwd=ROOT, check=False)
    return path, completed.returncode


def main() -> int:
    args = parse_arguments()
    paths = changed_files(args.base_ref)
    if requires_full_check(paths):
        print("Headers or build configuration changed; full clang-tidy is required.")
        return 2

    database = compile_commands(args.build_directory)
    sources = [
        (ROOT / path).resolve()
        for path in paths
        if path.suffix in SOURCE_SUFFIXES and (ROOT / path).resolve() in database
    ]
    if not sources:
        print("No changed translation units require clang-tidy.")
        return 0

    print(f"Running clang-tidy for {len(sources)} changed translation unit(s).")
    failures: list[Path] = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
        for path, returncode in executor.map(
            lambda source: run_clang_tidy(source, args.build_directory), sources
        ):
            if returncode != 0:
                failures.append(path)

    if failures:
        for path in failures:
            print(f"clang-tidy failed: {path.relative_to(ROOT)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
