#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
from typing import Any

if __package__:
    from tools import read_performance as common
    from tools import read_compatibility
else:
    import read_performance as common
    import read_compatibility


HarnessError = common.HarnessError
PerformanceMismatch = common.PerformanceMismatch

FIXTURE_ID = "indexed"
PAGE_SIZE = 4096
PAGE_COUNT = 206
ROW_COUNT = 4096
PAYLOAD_SIZE = 128
APPLICATION_ID = 1_297_305_937
USER_VERSION = 1
MINIMUM_WALL_NS = 5_000_000

CASE_IDS = (
    "index-equality-covering-hit",
    "index-equality-covering-miss",
    "index-equality-noncovering-hit",
    "index-multi-equality-covering",
    "index-range-covering",
    "index-range-noncovering",
    "index-unselective-noncovering",
    "index-unselective-covering",
)

_TOP_KEYS = {
    "schema_version",
    "workload_semantics_version",
    "sqlite_profile",
    "sqlite_semantic_compile_options",
    "configuration",
    "minimum_wall_ns",
    "permutation",
    "rounds",
    "fixtures",
    "cases",
    "guard",
}
_FIXTURE_KEYS = {
    "id",
    "path",
    "sql_path",
    "sha256",
    "sql_sha256",
    "size_bytes",
    "page_size",
    "page_count",
    "row_count",
    "payload_size",
}
_CASE_KEYS = {
    "id",
    "fixture",
    "kind",
    "sql",
    "primary_unit",
    "warmup_iterations",
    "measured_iterations",
    "diagnostic_iterations",
    "items_per_iteration",
    "expected",
}
_EXPECTED_KEYS = {"warmup", "measured", "diagnostic", "smoke", "verification"}
_CASE_CONTRACTS = {
    "index-equality-covering-hit": (
        "index",
        "SELECT id,score FROM items WHERE category=?1",
        "lookup",
        128,
        4096,
        128,
        1,
    ),
    "index-equality-covering-miss": (
        "index",
        "SELECT id,score FROM items WHERE category=?1",
        "lookup",
        128,
        4096,
        128,
        1,
    ),
    "index-equality-noncovering-hit": (
        "index",
        "SELECT payload FROM items WHERE category=?1",
        "lookup",
        128,
        4096,
        128,
        1,
    ),
    "index-multi-equality-covering": (
        "index",
        "SELECT id FROM items WHERE category=?1 AND score=?2",
        "lookup",
        128,
        4096,
        128,
        1,
    ),
    "index-range-covering": (
        "index",
        "SELECT id,score FROM items WHERE category>=?1 AND category<?2",
        "range",
        2,
        2048,
        2,
        64,
    ),
    "index-range-noncovering": (
        "index",
        "SELECT payload FROM items WHERE category>=?1 AND category<?2",
        "range",
        2,
        2048,
        2,
        64,
    ),
    "index-unselective-noncovering": (
        "scan",
        "SELECT payload FROM items WHERE flag=?1",
        "traversal",
        1,
        32,
        1,
        2048,
    ),
    "index-unselective-covering": (
        "index",
        "SELECT id FROM items WHERE flag=?1",
        "traversal",
        1,
        256,
        1,
        2048,
    ),
}

_COMMON_COLLECT_INPUT_REFERENCES = common._collect_input_references


def create_fixture(
    *,
    profile_path: pathlib.Path,
    sqlite_library_path: pathlib.Path,
    sqlite_c_path: pathlib.Path,
    sqlite_h_path: pathlib.Path,
    sql_path: pathlib.Path,
    output_path: pathlib.Path,
) -> dict[str, Any]:
    inputs = (
        profile_path,
        sqlite_library_path,
        sqlite_c_path,
        sqlite_h_path,
        sql_path,
        pathlib.Path(__file__),
    )
    for path in inputs:
        if not path.is_file():
            raise HarnessError(f"index fixture input is not a file: {path}")
    common.validate_new_output_path(output_path, protected_paths=list(inputs))
    profile = common._validate_profile(
        common.load_json_strict(profile_path),
        label="SQLite profile",
    )
    try:
        sql = sql_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise HarnessError(f"cannot read index fixture SQL {sql_path}: {error}") from error
    if not sql or "\0" in sql:
        raise HarnessError("index fixture SQL must be nonempty UTF-8 without NUL bytes")

    sidecars = tuple(
        pathlib.Path(f"{output_path}{suffix}")
        for suffix in ("-journal", "-wal", "-shm")
    )
    if any(os.path.lexists(path) for path in sidecars):
        raise HarnessError("index fixture sidecar path already exists")

    oracle = common._load_pinned_sqlite(
        library_path=sqlite_library_path,
        profile=profile,
        sqlite_c_path=sqlite_c_path,
        sqlite_h_path=sqlite_h_path,
    )
    try:
        oracle.create_database(output_path, sql)
    except read_compatibility.HarnessError as error:
        raise HarnessError(str(error)) from error
    if any(os.path.lexists(path) for path in sidecars):
        raise HarnessError("index fixture generation left a sidecar file")

    try:
        header = output_path.read_bytes()[:100]
        size_bytes = output_path.stat().st_size
    except OSError as error:
        raise HarnessError(f"cannot inspect generated index fixture: {error}") from error
    if len(header) != 100 or header[:16] != b"SQLite format 3\0":
        raise HarnessError("generated index fixture has an invalid SQLite header")
    encoded_page_size = int.from_bytes(header[16:18], "big")
    page_size = 65536 if encoded_page_size == 1 else encoded_page_size
    page_count = size_bytes // page_size if page_size != 0 else 0
    if (
        page_size != PAGE_SIZE
        or size_bytes != PAGE_SIZE * PAGE_COUNT
        or page_count != PAGE_COUNT
        or int.from_bytes(header[28:32], "big") != PAGE_COUNT
    ):
        raise HarnessError("generated index fixture page layout is not canonical")

    database, result = oracle._open(output_path, common._SQLITE_OPEN_READONLY)
    if result != common._SQLITE_OK:
        oracle._close(database)
        raise HarnessError(f"cannot reopen generated index fixture with SQLite code {result}")
    try:
        oracle._configure_connection(database)
        expected_objects = (
            "index:items_category_score:items|"
            "index:items_flag:items|"
            "index:items_score_desc:items|"
            "table:items:items"
        )
        objects = common._sqlite_single_text(
            oracle,
            database,
            "SELECT group_concat(type||':'||name||':'||tbl_name,'|') FROM ("
            "SELECT type,name,tbl_name FROM sqlite_schema "
            "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name)",
        )
        flag_stat = common._sqlite_single_text(
            oracle,
            database,
            "SELECT stat FROM sqlite_stat1 WHERE idx='items_flag'",
        )
        if (
            common._sqlite_single_text(oracle, database, "PRAGMA integrity_check")
            != "ok"
            or common._sqlite_single_text(oracle, database, "PRAGMA journal_mode").lower()
            != "delete"
            or common._sqlite_single_text(oracle, database, "PRAGMA encoding")
            != "UTF-8"
            or common._sqlite_single_integer(oracle, database, "PRAGMA page_size")
            != PAGE_SIZE
            or common._sqlite_single_integer(oracle, database, "PRAGMA page_count")
            != PAGE_COUNT
            or common._sqlite_single_integer(oracle, database, "PRAGMA auto_vacuum")
            != 0
            or common._sqlite_single_integer(oracle, database, "PRAGMA application_id")
            != APPLICATION_ID
            or common._sqlite_single_integer(oracle, database, "PRAGMA user_version")
            != USER_VERSION
            or common._sqlite_single_integer(
                oracle,
                database,
                "SELECT count(*) FROM items",
            )
            != ROW_COUNT
            or common._sqlite_single_text(
                oracle,
                database,
                "SELECT min(length(payload))||':'||max(length(payload)) FROM items",
            )
            != f"{PAYLOAD_SIZE}:{PAYLOAD_SIZE}"
            or objects != expected_objects
            or flag_stat != "4096 4096"
        ):
            raise HarnessError("generated index fixture schema or contents are not canonical")
    finally:
        oracle._close(database)

    return {
        "id": FIXTURE_ID,
        "path": output_path.name,
        "sql_path": sql_path.name,
        "sha256": common._sha256(output_path),
        "sql_sha256": common._sha256(sql_path),
        "size_bytes": size_bytes,
        "page_size": PAGE_SIZE,
        "page_count": PAGE_COUNT,
        "row_count": ROW_COUNT,
        "payload_size": PAYLOAD_SIZE,
    }


def _require_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    common._require_type(value, dict, label)
    common._require_exact_keys(value, expected, label)


def validate_workload_manifest(
    value: Any,
    *,
    repository_root: pathlib.Path,
    manifest_path: pathlib.Path,
) -> dict[str, Any]:
    _require_keys(value, _TOP_KEYS, "index workload manifest")
    if value["schema_version"] != 1 or value["workload_semantics_version"] != 1:
        raise HarnessError("index workload schema versions must be 1")
    try:
        manifest_path.resolve().relative_to(repository_root.resolve())
    except ValueError as error:
        raise HarnessError("index workload manifest escapes the repository") from error

    profile_path = common._resolve_repository_file(
        repository_root,
        value["sqlite_profile"],
        "index workload sqlite_profile",
    )
    profile = common._validate_profile(
        common.load_json_strict(profile_path),
        label="SQLite profile",
    )
    if value["sqlite_semantic_compile_options"] != profile["build"][
        "semantic_compile_options"
    ]:
        raise HarnessError("index workload compile options do not match the profile")

    expected_configuration = {
        "page_size": PAGE_SIZE,
        "cache_pages": 512,
        "mmap_bytes": 0,
        "temp_store": "memory",
        "synchronous": "full",
        "journal_mode": "delete",
        "query_only": True,
        "thread_mode": "single",
    }
    if value["configuration"] != expected_configuration:
        raise HarnessError("index workload configuration is not pinned")
    if value["minimum_wall_ns"] != MINIMUM_WALL_NS:
        raise HarnessError("index workload minimum wall duration is not pinned")
    if value["permutation"] != {
        "algorithm": "splitmix64-rejection-fisher-yates-v1",
        "fingerprint": "fnv1a64-v1",
        "fit_seed": "9e3779b97f4a7c15",
        "pressure_seed": "d1b54a32d192ed03",
    }:
        raise HarnessError("index workload permutation contract is not pinned")
    expected_rounds = [
        {
            "index": 0,
            "case_order": "canonical",
            "engine_order": ["modern", "sqlite"],
        },
        {
            "index": 1,
            "case_order": "reverse",
            "engine_order": ["sqlite", "modern"],
        },
        {
            "index": 2,
            "case_order": "canonical",
            "engine_order": ["modern", "sqlite"],
        },
    ]
    if value["rounds"] != expected_rounds:
        raise HarnessError("index workload round schedule is not pinned")

    fixtures = value["fixtures"]
    common._require_type(fixtures, list, "index workload fixtures")
    if len(fixtures) != 1:
        raise HarnessError("index workload must contain one fixture")
    fixture = fixtures[0]
    _require_keys(fixture, _FIXTURE_KEYS, "index workload fixture")
    if fixture["id"] != "fit":
        raise HarnessError("index workload fixture ID must be fit")
    database = common._resolve_repository_file(
        repository_root,
        fixture["path"],
        "index workload fixture path",
    )
    sql = common._resolve_repository_file(
        repository_root,
        fixture["sql_path"],
        "index workload fixture SQL path",
    )
    if (
        fixture["sha256"] != common._sha256(database)
        or fixture["sql_sha256"] != common._sha256(sql)
        or fixture["size_bytes"] != database.stat().st_size
        or fixture["page_size"] != PAGE_SIZE
        or fixture["page_count"] != PAGE_COUNT
        or fixture["row_count"] != ROW_COUNT
        or fixture["payload_size"] != PAYLOAD_SIZE
    ):
        raise HarnessError("index workload fixture metadata does not match")

    cases = value["cases"]
    common._require_type(cases, list, "index workload cases")
    if tuple(case.get("id") for case in cases) != CASE_IDS:
        raise HarnessError(f"index workload case IDs must be {list(CASE_IDS)}")
    for index, case in enumerate(cases):
        label = f"index workload cases[{index}]"
        _require_keys(case, _CASE_KEYS, label)
        contract = _CASE_CONTRACTS[case["id"]]
        exact = {
            "fixture": "fit",
            "kind": contract[0],
            "sql": contract[1],
            "primary_unit": contract[2],
            "warmup_iterations": contract[3],
            "measured_iterations": contract[4],
            "diagnostic_iterations": contract[5],
            "items_per_iteration": contract[6],
        }
        for key, expected in exact.items():
            if case[key] != expected:
                raise HarnessError(
                    f"{label}.{key} must be {expected!r} for {case['id']}"
                )
        expected_groups = case["expected"]
        _require_keys(expected_groups, _EXPECTED_KEYS, f"{label}.expected")
        for group in sorted(_EXPECTED_KEYS):
            common._validate_work(
                expected_groups[group],
                f"{label}.expected.{group}",
            )
        measured = expected_groups["measured"]
        if (
            measured["operations"] != case["measured_iterations"]
            or measured["items"]
            != case["measured_iterations"] * case["items_per_iteration"]
        ):
            raise HarnessError(f"{label} measured work does not match its scale")

    guard = value["guard"]
    _require_keys(guard, common._GUARD_KEYS, "index workload guard")
    for key in sorted(common._GUARD_KEYS):
        if guard[key] != {"numerator": 10, "denominator": 1}:
            raise HarnessError(f"index workload guard {key} must be exactly 10/1")
    return value


def _smoke_work(
    workload_manifest: dict[str, Any],
    case: dict[str, Any],
) -> dict[str, Any]:
    del workload_manifest
    return case["expected"]["smoke"]


def _collect_input_references(
    *,
    repository_root: pathlib.Path,
    workload_path: pathlib.Path,
    workload_manifest: dict[str, Any],
) -> list[dict[str, Any]]:
    references = _COMMON_COLLECT_INPUT_REFERENCES(
        repository_root=repository_root,
        workload_path=workload_path,
        workload_manifest=workload_manifest,
    )
    references.append(
        common._repository_input_reference(
            role="index-runner-source",
            path=repository_root / "tools/index_performance.py",
            repository_root=repository_root,
        )
    )
    return references


def _configure_common() -> None:
    common.EXPECTED_CASE_IDS = CASE_IDS
    common.MINIMUM_WALL_NS = MINIMUM_WALL_NS
    common.MAX_BASELINE_ARTIFACTS = 132
    common.ENFORCE_GUARD_ON_VALIDATION = False
    common.validate_workload_manifest = validate_workload_manifest
    common._smoke_work = _smoke_work
    common._collect_input_references = _collect_input_references


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    create = subparsers.add_parser("create-fixture")
    create.add_argument("--profile", type=pathlib.Path, required=True)
    create.add_argument("--sqlite-library", type=pathlib.Path, required=True)
    create.add_argument("--sqlite-c", type=pathlib.Path, required=True)
    create.add_argument("--sqlite-h", type=pathlib.Path, required=True)
    create.add_argument("--sql", type=pathlib.Path, required=True)
    create.add_argument("--output", type=pathlib.Path, required=True)
    validate_workloads = subparsers.add_parser("validate-workloads")
    validate_workloads.add_argument(
        "--repository-root",
        type=pathlib.Path,
        required=True,
    )
    validate_workloads.add_argument("--workloads", type=pathlib.Path, required=True)
    generate = subparsers.add_parser("generate-baseline")
    generate.add_argument("--repository-root", type=pathlib.Path, required=True)
    generate.add_argument("--workloads", type=pathlib.Path, required=True)
    generate.add_argument("--timing-binary", type=pathlib.Path, required=True)
    generate.add_argument("--diagnostic-binary", type=pathlib.Path, required=True)
    generate.add_argument("--sqlite-c", type=pathlib.Path, required=True)
    generate.add_argument("--sqlite-h", type=pathlib.Path, required=True)
    generate.add_argument("--output", type=pathlib.Path, required=True)
    validate = subparsers.add_parser("validate-baseline")
    validate.add_argument("--repository-root", type=pathlib.Path, required=True)
    validate.add_argument("--workloads", type=pathlib.Path, required=True)
    validate.add_argument("--baseline", type=pathlib.Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    try:
        _configure_common()
        arguments = parser.parse_args(argv)
        if arguments.command == "create-fixture":
            metadata = create_fixture(
                profile_path=arguments.profile.resolve(),
                sqlite_library_path=arguments.sqlite_library.resolve(),
                sqlite_c_path=arguments.sqlite_c.resolve(),
                sqlite_h_path=arguments.sqlite_h.resolve(),
                sql_path=arguments.sql.resolve(),
                output_path=arguments.output.resolve(),
            )
            print(json.dumps(metadata, sort_keys=True, separators=(",", ":")))
            return 0
        if arguments.command == "validate-workloads":
            validate_workload_manifest(
                common.load_json_strict(arguments.workloads.resolve()),
                repository_root=arguments.repository_root.resolve(),
                manifest_path=arguments.workloads.resolve(),
            )
            return 0
        if arguments.command == "generate-baseline":
            try:
                aggregate = common.generate_baseline(
                    repository_root=arguments.repository_root,
                    workload_path=arguments.workloads,
                    timing_binary_path=arguments.timing_binary,
                    diagnostic_binary_path=arguments.diagnostic_binary,
                    sqlite_c_path=arguments.sqlite_c,
                    sqlite_h_path=arguments.sqlite_h,
                    output_path=arguments.output,
                )
            except PerformanceMismatch:
                failure_path = arguments.output.resolve() / "failure.json"
                if failure_path.exists():
                    failure_path.unlink()
                aggregate = common.validate_baseline_directory(
                    baseline_path=arguments.output,
                    repository_root=arguments.repository_root,
                    workload_path=arguments.workloads,
                )
                print(
                    json.dumps(
                        {
                            "guard_passed": aggregate["guard_passed"],
                            "output": str(arguments.output),
                        },
                        sort_keys=True,
                        separators=(",", ":"),
                    )
                )
                return 2
            print(
                json.dumps(
                    {
                        "guard_passed": aggregate["guard_passed"],
                        "output": str(arguments.output),
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                )
            )
            return 0
        if arguments.command == "validate-baseline":
            common.validate_baseline_directory(
                baseline_path=arguments.baseline,
                repository_root=arguments.repository_root,
                workload_path=arguments.workloads,
            )
            return 0
        raise HarnessError(f"unsupported command: {arguments.command}")
    except (HarnessError, OSError) as error:
        print(f"index performance harness error: {error}", file=sys.stderr)
        return 1
    except PerformanceMismatch as error:
        print(f"index performance mismatch: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
