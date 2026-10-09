#!/usr/bin/env python3

from __future__ import annotations

import argparse
import copy
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
_COMMON_VALIDATE_RAW_DIAGNOSTIC_REPORT = common.validate_raw_diagnostic_report
_COMMON_VALIDATE_RAW_TIMING_REPORT = common.validate_raw_timing_report

PAGE_SIZE = 4096
FIXTURE_ID = "ordered"
PAGE_COUNT = 4866
ROW_COUNT = 65536
PAYLOAD_SIZE = 256
APPLICATION_ID = 1_297_305_426
USER_VERSION = 1
MINIMUM_WALL_NS = 5_000_000

CASE_IDS = (
    "order-integer-memory",
    "order-mixed-collated-memory",
    "order-desc-null-memory",
    "order-noncovering-payload-memory",
    "order-minimal-spill-file",
    "order-multi-spill-file",
    "order-full-sort-limit-memory",
    "order-topn-memory",
    "order-topn-file",
    "order-index-compatible-before",
)
ORDER_SQLITE_COUNTER_NAMES = common.SQLITE_COUNTER_NAMES + (
    "sort_operations",
    "temp_bytes_spilled",
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
    "result_rows_per_iteration",
    "query_only",
    "temporary_store",
    "sorter_memory_threshold",
    "expected",
}
_EXPECTED_KEYS = {"warmup", "measured", "diagnostic", "smoke", "verification"}
_CASE_CONTRACTS = {
    "order-integer-memory": (
        "SELECT id,score FROM items ORDER BY score+0,id",
        65536,
        "memory",
        64 << 20,
    ),
    "order-mixed-collated-memory": (
        "SELECT id,category,score FROM items "
        "ORDER BY iif(flag=0,category,score) COLLATE NOCASE,id",
        65536,
        "memory",
        64 << 20,
    ),
    "order-desc-null-memory": (
        "SELECT id,score FROM items "
        "ORDER BY nullif(score,0) DESC NULLS FIRST,id DESC",
        65536,
        "memory",
        64 << 20,
    ),
    "order-noncovering-payload-memory": (
        "SELECT id,payload FROM items ORDER BY category DESC,id",
        65536,
        "memory",
        64 << 20,
    ),
    "order-minimal-spill-file": (
        "SELECT id,payload FROM items ORDER BY category,id",
        65536,
        "file",
        10 << 20,
    ),
    "order-multi-spill-file": (
        "SELECT id,payload FROM items ORDER BY category,id",
        65536,
        "file",
        2 << 20,
    ),
    "order-full-sort-limit-memory": (
        "SELECT id,payload FROM items ORDER BY category,id LIMIT -1 OFFSET 16384",
        49152,
        "memory",
        64 << 20,
    ),
    "order-topn-memory": (
        "SELECT id,score FROM items ORDER BY category,id LIMIT 64 OFFSET 64",
        64,
        "memory",
        64 << 20,
    ),
    "order-topn-file": (
        "SELECT id,payload FROM items ORDER BY payload,id LIMIT 64 OFFSET 60000",
        64,
        "file",
        2 << 20,
    ),
    "order-index-compatible-before": (
        "SELECT id,payload FROM items ORDER BY score DESC,id",
        65536,
        "memory",
        64 << 20,
    ),
}


def _require_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    common._require_type(value, dict, label)
    common._require_exact_keys(value, expected, label)


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
            raise HarnessError(f"ORDER BY fixture input is not a file: {path}")
    common.validate_new_output_path(output_path, protected_paths=list(inputs))
    profile = common._validate_profile(
        common.load_json_strict(profile_path),
        label="SQLite profile",
    )
    try:
        sql = sql_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise HarnessError(f"cannot read ORDER BY fixture SQL {sql_path}: {error}") from error
    if not sql or "\0" in sql:
        raise HarnessError("ORDER BY fixture SQL must be nonempty UTF-8 without NUL bytes")

    sidecars = tuple(
        pathlib.Path(f"{output_path}{suffix}")
        for suffix in ("-journal", "-wal", "-shm")
    )
    if any(os.path.lexists(path) for path in sidecars):
        raise HarnessError("ORDER BY fixture sidecar path already exists")

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
        raise HarnessError("ORDER BY fixture generation left a sidecar file")

    try:
        header = output_path.read_bytes()[:100]
        size_bytes = output_path.stat().st_size
    except OSError as error:
        raise HarnessError(f"cannot inspect generated ORDER BY fixture: {error}") from error
    if len(header) != 100 or header[:16] != b"SQLite format 3\0":
        raise HarnessError("generated ORDER BY fixture has an invalid SQLite header")
    encoded_page_size = int.from_bytes(header[16:18], "big")
    page_size = 65536 if encoded_page_size == 1 else encoded_page_size
    page_count = size_bytes // page_size if page_size != 0 else 0
    if (
        page_size != PAGE_SIZE
        or size_bytes != PAGE_SIZE * PAGE_COUNT
        or page_count != PAGE_COUNT
        or int.from_bytes(header[28:32], "big") != PAGE_COUNT
    ):
        raise HarnessError("generated ORDER BY fixture page layout is not canonical")

    database, result = oracle._open(output_path, common._SQLITE_OPEN_READONLY)
    if result != common._SQLITE_OK:
        oracle._close(database)
        raise HarnessError(
            f"cannot reopen generated ORDER BY fixture with SQLite code {result}"
        )
    try:
        oracle._configure_connection(database)
        objects = common._sqlite_single_text(
            oracle,
            database,
            "SELECT group_concat(type||':'||name||':'||tbl_name,'|') FROM ("
            "SELECT type,name,tbl_name FROM sqlite_schema "
            "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name)",
        )
        score_stat = common._sqlite_single_text(
            oracle,
            database,
            "SELECT stat FROM sqlite_stat1 WHERE idx='items_score_desc'",
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
            or objects != "index:items_score_desc:items|table:items:items"
            or score_stat != "65536 16"
        ):
            raise HarnessError("generated ORDER BY fixture schema or contents are not canonical")
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


def validate_workload_manifest(
    value: Any,
    *,
    repository_root: pathlib.Path,
    manifest_path: pathlib.Path,
) -> dict[str, Any]:
    _require_keys(value, _TOP_KEYS, "ORDER BY workload manifest")
    if value["schema_version"] != 1 or value["workload_semantics_version"] != 1:
        raise HarnessError("ORDER BY workload schema versions must be 1")
    try:
        manifest_path.resolve().relative_to(repository_root.resolve())
    except ValueError as error:
        raise HarnessError("ORDER BY workload manifest escapes the repository") from error

    profile_path = common._resolve_repository_file(
        repository_root,
        value["sqlite_profile"],
        "ORDER BY workload sqlite_profile",
    )
    profile = common._validate_profile(
        common.load_json_strict(profile_path),
        label="SQLite profile",
    )
    if value["sqlite_semantic_compile_options"] != profile["build"][
        "semantic_compile_options"
    ]:
        raise HarnessError("ORDER BY workload compile options do not match the profile")

    expected_configuration = {
        "page_size": PAGE_SIZE,
        "cache_pages": 512,
        "mmap_bytes": 0,
        "temp_store": "case",
        "sorter_memory_threshold": "case",
        "merge_fan_in": 16,
        "synchronous": "full",
        "journal_mode": "delete",
        "query_only": True,
        "thread_mode": "single",
    }
    if value["configuration"] != expected_configuration:
        raise HarnessError("ORDER BY workload configuration is not pinned")
    if value["minimum_wall_ns"] != MINIMUM_WALL_NS:
        raise HarnessError("ORDER BY workload minimum wall duration is not pinned")
    if value["permutation"] != {
        "algorithm": "splitmix64-rejection-fisher-yates-v1",
        "fingerprint": "fnv1a64-v1",
        "fit_seed": "9e3779b97f4a7c15",
        "pressure_seed": "d1b54a32d192ed03",
    }:
        raise HarnessError("ORDER BY workload permutation contract is not pinned")
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
        raise HarnessError("ORDER BY workload round schedule is not pinned")

    fixtures = value["fixtures"]
    common._require_type(fixtures, list, "ORDER BY workload fixtures")
    if len(fixtures) != 1:
        raise HarnessError("ORDER BY workload must contain one fixture")
    fixture = fixtures[0]
    _require_keys(fixture, _FIXTURE_KEYS, "ORDER BY workload fixture")
    if fixture["id"] != FIXTURE_ID:
        raise HarnessError(f"ORDER BY workload fixture ID must be {FIXTURE_ID}")
    database = common._resolve_repository_file(
        repository_root,
        fixture["path"],
        "ORDER BY workload fixture path",
    )
    sql = common._resolve_repository_file(
        repository_root,
        fixture["sql_path"],
        "ORDER BY workload fixture SQL path",
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
        raise HarnessError("ORDER BY workload fixture metadata does not match")

    cases = value["cases"]
    common._require_type(cases, list, "ORDER BY workload cases")
    for index, case in enumerate(cases):
        common._require_type(case, dict, f"ORDER BY workload cases[{index}]")
    if tuple(case.get("id") for case in cases) != CASE_IDS:
        raise HarnessError(f"ORDER BY workload case IDs must be {list(CASE_IDS)}")
    for index, case in enumerate(cases):
        label = f"ORDER BY workload cases[{index}]"
        _require_keys(case, _CASE_KEYS, label)
        sql_text, result_rows, temp_store, threshold = _CASE_CONTRACTS[case["id"]]
        measured_iterations = 2 if case["id"] == "order-topn-memory" else 1
        exact = {
            "fixture": FIXTURE_ID,
            "kind": "order-by",
            "sql": sql_text,
            "primary_unit": "source-row",
            "warmup_iterations": 1,
            "measured_iterations": measured_iterations,
            "diagnostic_iterations": 1,
            "items_per_iteration": ROW_COUNT,
            "result_rows_per_iteration": result_rows,
            "query_only": True,
            "temporary_store": temp_store,
            "sorter_memory_threshold": threshold,
        }
        for key, expected in exact.items():
            if case[key] != expected:
                raise HarnessError(
                    f"{label}.{key} must be {expected!r} for {case['id']}"
                )
        expected_groups = case["expected"]
        _require_keys(expected_groups, _EXPECTED_KEYS, f"{label}.expected")
        for group in sorted(_EXPECTED_KEYS):
            common._validate_work(expected_groups[group], f"{label}.expected.{group}")
        for group, iterations in (
            ("warmup", 1),
            ("smoke", 1),
            ("diagnostic", 1),
            ("measured", measured_iterations),
        ):
            work = expected_groups[group]
            if (
                work["operations"] != iterations
                or work["items"] != iterations * ROW_COUNT
                or work["rows"] != iterations * result_rows
                or work["result_hits"] != iterations
                or work["result_misses"] != 0
            ):
                raise HarnessError(f"{label}.expected.{group} does not match its scale")

    guard = value["guard"]
    _require_keys(guard, common._GUARD_KEYS, "ORDER BY workload guard")
    for key in sorted(common._GUARD_KEYS):
        if guard[key] != {"numerator": 10, "denominator": 1}:
            raise HarnessError(f"ORDER BY workload guard {key} must be exactly 10/1")
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
    profile_path = common._resolve_repository_file(
        repository_root,
        workload_manifest["sqlite_profile"],
        "ORDER BY workload sqlite_profile",
    )
    references = [
        common._repository_input_reference(
            role="workload-manifest",
            path=workload_path,
            repository_root=repository_root,
        ),
        common._repository_input_reference(
            role="sqlite-profile",
            path=profile_path,
            repository_root=repository_root,
        ),
        common._repository_input_reference(
            role="benchmark-source",
            path=repository_root / "benchmarks/order_by_performance.cpp",
            repository_root=repository_root,
        ),
        common._repository_input_reference(
            role="shared-runner-source",
            path=repository_root / "tools/read_performance.py",
            repository_root=repository_root,
        ),
        common._repository_input_reference(
            role="order-by-runner-source",
            path=repository_root / "tools/order_by_performance.py",
            repository_root=repository_root,
        ),
    ]
    for fixture in workload_manifest["fixtures"]:
        references.append(
            common._repository_input_reference(
                role=f"fixture-{fixture['id']}",
                path=repository_root / pathlib.PurePosixPath(fixture["path"]),
                repository_root=repository_root,
            )
        )
        references.append(
            common._repository_input_reference(
                role=f"fixture-sql-{fixture['id']}",
                path=repository_root / pathlib.PurePosixPath(fixture["sql_path"]),
                repository_root=repository_root,
            )
        )
    return references


def _expected_case_configuration(
    workload_manifest: dict[str, Any],
    case: dict[str, Any],
) -> dict[str, Any]:
    configuration = dict(workload_manifest["configuration"])
    configuration["temp_store"] = case["temporary_store"]
    configuration["sorter_memory_threshold"] = case["sorter_memory_threshold"]
    return configuration


def validate_raw_timing_report(
    value: Any,
    *,
    workload_manifest: dict[str, Any],
    expected_engine: str,
    expected_case: str,
    expected_run_kind: str = "baseline",
) -> dict[str, Any]:
    case = common._case_by_id(workload_manifest, expected_case)
    if not isinstance(value, dict) or not isinstance(
        value.get("effective_configuration"), dict
    ):
        return _COMMON_VALIDATE_RAW_TIMING_REPORT(
            value,
            workload_manifest=workload_manifest,
            expected_engine=expected_engine,
            expected_case=expected_case,
            expected_run_kind=expected_run_kind,
        )
    normalized = copy.deepcopy(value)
    actual_configuration = normalized["effective_configuration"]
    normalized["effective_configuration"] = dict(workload_manifest["configuration"])
    _COMMON_VALIDATE_RAW_TIMING_REPORT(
        normalized,
        workload_manifest=workload_manifest,
        expected_engine=expected_engine,
        expected_case=expected_case,
        expected_run_kind=expected_run_kind,
    )
    expected_configuration = _expected_case_configuration(workload_manifest, case)
    if actual_configuration != expected_configuration:
        raise HarnessError(
            "timing report.effective_configuration does not match the ORDER BY case"
        )
    return value


def validate_raw_diagnostic_report(
    value: Any,
    *,
    workload_manifest: dict[str, Any],
    expected_engine: str,
    expected_case: str,
) -> dict[str, Any]:
    if not isinstance(value, dict) or not isinstance(value.get("counters"), dict):
        return _COMMON_VALIDATE_RAW_DIAGNOSTIC_REPORT(
            value,
            workload_manifest=workload_manifest,
            expected_engine=expected_engine,
            expected_case=expected_case,
        )
    normalized = copy.deepcopy(value)
    actual_configuration = normalized.get("effective_configuration")
    normalized["effective_configuration"] = dict(workload_manifest["configuration"])
    selected = normalized["counters"].get(expected_engine)
    if expected_engine == "modern":
        common._validate_counter_object(
            selected,
            expected_names=common.MODERN_COUNTER_NAMES,
            label="diagnostic report.counters.modern",
        )
        selected["pages_written"] = 0
        selected["pages_read"] = 0
        selected["cache_misses"] = 0
    elif expected_engine == "sqlite":
        common._validate_counter_object(
            selected,
            expected_names=ORDER_SQLITE_COUNTER_NAMES,
            label="diagnostic report.counters.sqlite",
        )
        selected.pop("sort_operations")
        selected.pop("temp_bytes_spilled")
        selected["cache_writes"] = 0
        selected["cache_misses"] = 0
        selected["fullscan_steps"] = 0
    else:
        raise HarnessError("expected engine must be modern or sqlite")
    _COMMON_VALIDATE_RAW_DIAGNOSTIC_REPORT(
        normalized,
        workload_manifest=workload_manifest,
        expected_engine=expected_engine,
        expected_case=expected_case,
    )
    case = common._case_by_id(workload_manifest, expected_case)
    expected_configuration = _expected_case_configuration(workload_manifest, case)
    if actual_configuration != expected_configuration:
        raise HarnessError(
            "diagnostic report.effective_configuration does not match the ORDER BY case"
        )
    counters = value["counters"][expected_engine]
    if expected_engine == "modern":
        if counters["pages_read"] != counters["cache_misses"]:
            raise HarnessError(
                "Modern ORDER BY diagnostic page reads and cache misses must match"
            )
    elif expected_engine == "sqlite":
        if counters["fullscan_steps"] != ROW_COUNT - 1:
            raise HarnessError(
                "SQLite ORDER BY diagnostic must scan every source row"
            )
        expected_sorts = 0 if expected_case == "order-index-compatible-before" else 1
        if counters["sort_operations"] != expected_sorts:
            raise HarnessError(
                "SQLite ORDER BY diagnostic sort count does not match the case"
            )
        expects_spill = expected_case in {
            "order-minimal-spill-file",
            "order-multi-spill-file",
        }
        if (counters["temp_bytes_spilled"] > 0) != expects_spill:
            raise HarnessError(
                "SQLite ORDER BY diagnostic spill bytes do not match the case"
            )
    else:
        raise HarnessError("expected engine must be modern or sqlite")
    return value


def _capture_actual_build_flags(
    *,
    cache: dict[str, str],
    repository_root: pathlib.Path,
    build_directory: pathlib.Path,
    sqlite_source_directory: pathlib.Path,
) -> dict[str, str]:
    if cache["CMAKE_GENERATOR"] != "Ninja":
        raise HarnessError("benchmark provenance requires the pinned Ninja generator")
    compile_commands = common.load_json_strict(build_directory / "compile_commands.json")
    common._require_type(compile_commands, list, "benchmark compile_commands.json")
    targets = {
        "actual_sqlite_c_compile_flags": (
            "CMakeFiles/modern_sqlite_benchmark_sqlite.dir/",
            True,
        ),
        "actual_modern_cxx_compile_flags": ("CMakeFiles/modern_sqlite.dir/", True),
        "actual_harness_cxx_compile_flags": (
            "CMakeFiles/modern_sqlite_order_by_benchmark.dir/",
            True,
        ),
    }
    captured: dict[str, str] = {}
    for field, (needle, require_optimization) in targets.items():
        flags = set()
        for index, entry in enumerate(compile_commands):
            common._require_type(entry, dict, f"benchmark compile command {index}")
            command = common._require_string(
                entry.get("command"),
                f"benchmark compile command {index}.command",
            )
            if needle not in command:
                continue
            extracted = common._extract_compile_flags(
                command,
                repository_root=repository_root,
                build_directory=build_directory,
                sqlite_source_directory=sqlite_source_directory,
            )
            common._validate_benchmark_flags(
                extracted,
                require_optimization=require_optimization,
                label=field,
            )
            flags.add(extracted)
        if not flags:
            raise HarnessError(f"no actual compile commands found for {field}")
        captured[field] = "\n".join(sorted(flags))

    make_program = cache.get("CMAKE_MAKE_PROGRAM")
    if not make_program:
        raise HarnessError("CMake cache is missing CMAKE_MAKE_PROGRAM")
    target = "modern_sqlite_order_by_benchmark"
    commands = common._command_output(
        [make_program, "-C", str(build_directory), "-t", "commands", target],
        cwd=repository_root,
        label="Ninja ORDER BY timing-target command lookup",
    ).decode("utf-8", errors="strict")
    link_candidates = [
        line
        for line in commands.splitlines()
        if target in line
        and " -c " not in line
        and (f" -o {target} " in line or f"/OUT:{target}".upper() in line.upper())
    ]
    if len(link_candidates) != 1:
        raise HarnessError(
            "Ninja command graph must contain one ORDER BY timing executable link command"
        )
    link_flags = common._extract_link_flags(
        link_candidates[0],
        compiler_path=cache["CMAKE_CXX_COMPILER"],
        output_name=target,
        repository_root=repository_root,
        build_directory=build_directory,
        sqlite_source_directory=sqlite_source_directory,
    )
    common._validate_benchmark_flags(
        link_flags,
        require_optimization=False,
        label="actual_timing_link_flags",
    )
    captured["actual_timing_link_flags"] = link_flags
    return captured


def _configure_common() -> None:
    common.EXPECTED_CASE_IDS = CASE_IDS
    common.MINIMUM_WALL_NS = MINIMUM_WALL_NS
    common.MAX_BASELINE_ARTIFACTS = 200
    common.ENFORCE_GUARD_ON_VALIDATION = True
    common.validate_workload_manifest = validate_workload_manifest
    common._smoke_work = _smoke_work
    common._collect_input_references = _collect_input_references
    common.validate_raw_timing_report = validate_raw_timing_report
    common.validate_raw_diagnostic_report = validate_raw_diagnostic_report
    common._capture_actual_build_flags = _capture_actual_build_flags


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
    validate_workloads.add_argument("--repository-root", type=pathlib.Path, required=True)
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
        print(f"ORDER BY performance harness error: {error}", file=sys.stderr)
        return 1
    except PerformanceMismatch as error:
        print(f"ORDER BY performance mismatch: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
