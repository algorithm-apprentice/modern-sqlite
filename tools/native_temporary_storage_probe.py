#!/usr/bin/env python3

import argparse
import contextlib
import json
import os
import pathlib
import sys
import tempfile
import time
from typing import BinaryIO, TextIO


ROUNDS = 50


def emit(output: TextIO, record: dict[str, object]) -> None:
    line = "native-storage-probe " + json.dumps(record, sort_keys=True) + "\n"
    if output.write(line) != len(line):
        raise OSError("could not publish native storage control")
    output.flush()


def write_and_sync(file: BinaryIO, data: bytes, output: TextIO,
                   role: str, index: int, operation: str) -> dict[str, int]:
    cpu = time.process_time_ns()
    before = time.perf_counter_ns()
    if file.write(data) != len(data):
        raise OSError("native storage control received a short write")
    written = time.perf_counter_ns()
    os.fsync(file.fileno())
    synced = time.perf_counter_ns()
    durations = {
        "write_ns": written - before,
        "sync_ns": synced - written,
        "cpu_ns": time.process_time_ns() - cpu,
    }
    if any(value < 0 for value in durations.values()):
        raise OSError("native storage control clock moved backwards")
    emit(output, {
        "phase": "sync", "role": role, "round": index, "operation": operation,
        "bytes": len(data), **durations,
    })
    return durations


def verify_bytes(file: BinaryIO, expected: bytes) -> None:
    file.seek(0)
    if file.read() != expected:
        raise OSError("native storage control byte verification failed")


def run_probe(system_root: pathlib.Path, runner_root: pathlib.Path, output: TextIO) -> None:
    roots = {"system": system_root.resolve(strict=True), "runner": runner_root.resolve(strict=True)}
    for root in roots.values():
        if not root.is_dir():
            raise ValueError(f"native storage control root is not a directory: {root}")
    totals = {
        role: {"writes": 0, "syncs": 0, "verified_bytes": 0,
               "write_ns": 0, "sync_ns": 0, "cpu_ns": 0}
        for role in roots
    }
    with contextlib.ExitStack() as owners:
        fixtures = {}
        for role, root in roots.items():
            child = pathlib.Path(owners.enter_context(tempfile.TemporaryDirectory(
                prefix="modern-sqlite-storage-probe-", dir=root,
            )))
            database = owners.enter_context((child / "main.bin").open("x+b", buffering=0))
            fixtures[role] = (child, database)
            emit(output, {
                "phase": "root", "role": role, "root": str(root), "owned_child": str(child),
                "volume": root.anchor, "device": root.stat().st_dev,
                "cpu_source": time.get_clock_info("process_time").implementation,
                "sync_api": "os.fsync", "rounds": ROUNDS,
            })
        for index in range(ROUNDS):
            roles = ("system", "runner") if index % 2 == 0 else ("runner", "system")
            header = bytes([index]) * 512
            payload = bytes([index]) * 4096
            for role in roles:
                child, database = fixtures[role]
                journal_path = child / "journal.bin"
                with journal_path.open("x+b", buffering=0) as journal:
                    database.seek(0)
                    for file, data, operation in (
                        (journal, header, "journal-header"),
                        (journal, payload, "journal-payload"),
                        (database, payload, "database-page"),
                    ):
                        durations = write_and_sync(file, data, output, role, index, operation)
                        totals[role]["writes"] += 1
                        totals[role]["syncs"] += 1
                        for key, value in durations.items():
                            totals[role][key] += value
                    verify_bytes(journal, header + payload)
                    verify_bytes(database, payload)
                    totals[role]["verified_bytes"] += len(header) + 2 * len(payload)
                journal_path.unlink()
    for role, total in totals.items():
        emit(output, {"phase": "root-complete", "role": role, **total})
    emit(output, {"phase": "complete", "roots": 2, "writes": 300, "syncs": 300})


def main() -> int:
    parser = argparse.ArgumentParser(description="Diagnose two unchanged native temporary roots")
    parser.add_argument("--system-root", required=True, type=pathlib.Path)
    parser.add_argument("--runner-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        run_probe(args.system_root, args.runner_root, sys.stdout)
    except (OSError, ValueError) as error:
        print(f"native storage control failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
