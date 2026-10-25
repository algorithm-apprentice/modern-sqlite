#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import copy
import dataclasses
import json
import pathlib
import sys
from typing import Any

if __package__:
    from tools import distinct_compound_performance as contracts
    from tools import order_by_performance as base
    from tools import read_performance as common
else:
    import distinct_compound_performance as contracts
    import order_by_performance as base
    import read_performance as common


HarnessError = common.HarnessError
MEASURED_ITERATIONS = 8
GUARD_RATIO = 10
MINIMUM_WALL_NS = 5_000_000
CONSTANT_VALUES = ",".join(str(value) for value in range(1, 257))
CONSTANT_SQL = f"SELECT id IN ({CONSTANT_VALUES}) FROM items ORDER BY id"


@dataclasses.dataclass(frozen=True)
class _Case:
    id: str
    sql: str
    bytes_per_iteration: int
    single_digest: str
    measured_digest: str
    file_backed: bool = False


CASE_DATA = (
    _Case(
        "in-linear-two-memory", "SELECT id IN (1,2) FROM items ORDER BY id",
        524288, "ccc0daeac801a306", "4ff28e4821364c65",
    ),
    _Case(
        "in-constant-256-memory", CONSTANT_SQL,
        524288, "1ee3d7fc6d58c3ee", "d603a57a7060d2e5",
    ),
    _Case(
        "in-constant-256-file", CONSTANT_SQL,
        524288, "1ee3d7fc6d58c3ee", "d603a57a7060d2e5", file_backed=True,
    ),
    _Case(
        "in-dynamic-three-memory", "SELECT id IN (1,id+1,NULL) FROM items ORDER BY id",
        8, "12d7ce1977232e8a", "2e7c35c20ee0afc5",
    ),
    _Case(
        "in-correlated-constant-memory",
        f"SELECT (SELECT i.id IN ({CONSTANT_VALUES})) FROM items i ORDER BY i.id",
        524288, "1ee3d7fc6d58c3ee", "d603a57a7060d2e5",
    ),
)
CASE_IDS = tuple(case.id for case in CASE_DATA)
_BASE_VALIDATE_MANIFEST = base.validate_workload_manifest
_BASE_VALIDATE_TIMING = base.validate_raw_timing_report
_BASE_VALIDATE_DIAGNOSTIC = base.validate_raw_diagnostic_report
_BASE_COLLECT_INPUTS = base._collect_input_references
_COMMON_FIELDS = (
    "EXPECTED_CASE_IDS", "MINIMUM_WALL_NS", "MAX_BASELINE_ARTIFACTS",
    "ENFORCE_GUARD_ON_VALIDATION", "validate_workload_manifest", "_smoke_work",
    "_collect_input_references", "validate_raw_timing_report",
    "validate_raw_diagnostic_report", "_capture_actual_build_flags",
)


def _work(case: _Case, *, measured: bool = False) -> dict[str, Any]:
    iterations = MEASURED_ITERATIONS if measured else 1
    return {
        "operations": iterations,
        "items": iterations * base.ROW_COUNT,
        "rows": iterations * base.ROW_COUNT,
        "bytes": iterations * case.bytes_per_iteration,
        "result_hits": iterations,
        "result_misses": 0,
        "digest": case.measured_digest if measured else case.single_digest,
    }


@contextlib.contextmanager
def _configured_cases() -> Any:
    previous = (base.CASE_IDS, base._CASE_CONTRACTS, base.MINIMUM_WALL_NS)
    base.CASE_IDS = CASE_IDS
    base._CASE_CONTRACTS = {
        case.id: (
            case.sql, base.ROW_COUNT, "file" if case.file_backed else "memory",
            (2 if case.file_backed else 64) << 20,
        )
        for case in CASE_DATA
    }
    base.MINIMUM_WALL_NS = MINIMUM_WALL_NS
    try:
        yield
    finally:
        base.CASE_IDS, base._CASE_CONTRACTS, base.MINIMUM_WALL_NS = previous


def validate_workload_manifest(
    value: Any, *, repository_root: pathlib.Path, manifest_path: pathlib.Path,
) -> dict[str, Any]:
    contracts._validate_original_manifest_types(value, guard_ratio=GUARD_RATIO)
    common._require_type(value["cases"], list, "IN workload cases")
    if len(value["cases"]) != len(CASE_DATA):
        raise HarnessError("IN workload case IDs are not pinned")
    for actual, case in zip(value["cases"], CASE_DATA):
        expected = {
            "id": case.id, "fixture": base.FIXTURE_ID, "kind": "order-by", "sql": case.sql,
            "primary_unit": "source-row", "warmup_iterations": 1,
            "measured_iterations": MEASURED_ITERATIONS, "diagnostic_iterations": 1,
            "items_per_iteration": base.ROW_COUNT,
            "result_rows_per_iteration": base.ROW_COUNT, "query_only": True,
            "temporary_store": "file" if case.file_backed else "memory",
            "sorter_memory_threshold": (2 if case.file_backed else 64) << 20,
            "expected": {
                "warmup": _work(case), "measured": _work(case, measured=True),
                "diagnostic": _work(case), "smoke": _work(case),
                "verification": contracts.VERIFICATION_WORK,
            },
        }
        contracts._require_exact_typed(actual, expected, f"IN workload {case.id}")
    normalized = copy.deepcopy(value)
    for case in normalized["cases"]:
        case["measured_iterations"] = 1
        case["expected"]["measured"] = dict(case["expected"]["smoke"])
    with _configured_cases():
        _BASE_VALIDATE_MANIFEST(
            normalized, repository_root=repository_root, manifest_path=manifest_path,
        )
    return value


def validate_raw_timing_report(
    value: Any, *, workload_manifest: dict[str, Any], expected_engine: str,
    expected_case: str, expected_run_kind: str = "baseline",
) -> dict[str, Any]:
    common._require_type(value, dict, "IN timing report")
    common._require_exact_keys(value, common._RAW_TIMING_KEYS, "IN timing report")
    contracts._validate_report_versions(
        value, ("schema_version", "completion_schema_version", "workload_semantics_version"),
        "IN timing report",
    )
    case = common._case_by_id(workload_manifest, expected_case)
    contracts._require_exact_typed(
        value["effective_configuration"], base._expected_case_configuration(workload_manifest, case),
        "IN timing configuration",
    )
    return _BASE_VALIDATE_TIMING(
        value, workload_manifest=workload_manifest, expected_engine=expected_engine,
        expected_case=expected_case, expected_run_kind=expected_run_kind,
    )


def validate_raw_diagnostic_report(
    value: Any, *, workload_manifest: dict[str, Any], expected_engine: str, expected_case: str,
) -> dict[str, Any]:
    common._require_type(value, dict, "IN diagnostic report")
    common._require_exact_keys(value, common._RAW_DIAGNOSTIC_KEYS, "IN diagnostic report")
    contracts._validate_report_versions(
        value, ("schema_version", "completion_schema_version", "diagnostic_schema_version",
                "workload_semantics_version"), "IN diagnostic report",
    )
    case = common._case_by_id(workload_manifest, expected_case)
    contracts._require_exact_typed(
        value["effective_configuration"], base._expected_case_configuration(workload_manifest, case),
        "IN diagnostic configuration",
    )
    normalized = value
    if expected_engine == "sqlite":
        common._require_type(value["counters"], dict, "IN diagnostic counters")
        counters = value["counters"].get("sqlite")
        common._require_type(counters, dict, "IN SQLite counters")
        contracts._require_exact_typed(counters.get("sort_operations"), 0, "IN SQLite sort count")
        normalized = copy.deepcopy(value)
        # The shared replay validator expects one sort, whereas these scans use rowid order.
        normalized["counters"]["sqlite"]["sort_operations"] = 1
    _BASE_VALIDATE_DIAGNOSTIC(
        normalized, workload_manifest=workload_manifest, expected_engine=expected_engine,
        expected_case=expected_case,
    )
    return value


def collect_input_references(
    *, repository_root: pathlib.Path, workload_path: pathlib.Path,
    workload_manifest: dict[str, Any],
) -> list[dict[str, Any]]:
    references = _BASE_COLLECT_INPUTS(
        repository_root=repository_root, workload_path=workload_path,
        workload_manifest=workload_manifest,
    )
    for role, relative in (
        ("in-list-runner-source", "tools/in_performance.py"),
        ("shared-contract-source", "tools/distinct_compound_performance.py"),
    ):
        references.append(common._repository_input_reference(
            role=role, path=repository_root / relative, repository_root=repository_root,
        ))
    return references


@contextlib.contextmanager
def configured_contract() -> Any:
    replacements = {
        "validate_workload_manifest": validate_workload_manifest,
        "validate_raw_timing_report": validate_raw_timing_report,
        "validate_raw_diagnostic_report": validate_raw_diagnostic_report,
        "_collect_input_references": collect_input_references,
    }
    previous_base = {name: getattr(base, name) for name in replacements}
    previous_common = {name: getattr(common, name) for name in _COMMON_FIELDS}
    with _configured_cases():
        try:
            for name, replacement in replacements.items():
                setattr(base, name, replacement)
            base._configure_common()
            yield
        finally:
            for name, previous in previous_base.items():
                setattr(base, name, previous)
            for name, previous in previous_common.items():
                setattr(common, name, previous)


def main(argv: list[str] | None = None) -> int:
    arguments = contracts._build_parser().parse_args(argv)
    try:
        with configured_contract():
            if arguments.command == "validate-workloads":
                validate_workload_manifest(
                    common.load_json_strict(arguments.workloads),
                    repository_root=arguments.repository_root, manifest_path=arguments.workloads,
                )
                return 0
            if arguments.command in {"validate-baseline", "validate-historical-baseline"}:
                common.validate_baseline_directory(
                    baseline_path=arguments.baseline, repository_root=arguments.repository_root,
                    workload_path=arguments.workloads,
                    verify_current_inputs=arguments.command == "validate-baseline",
                )
                return 0
            if arguments.command == "generate-baseline":
                result = common.generate_baseline(
                    repository_root=arguments.repository_root, workload_path=arguments.workloads,
                    timing_binary_path=arguments.timing_binary,
                    diagnostic_binary_path=arguments.diagnostic_binary,
                    sqlite_c_path=arguments.sqlite_c, sqlite_h_path=arguments.sqlite_h,
                    output_path=arguments.output,
                )
                print(json.dumps({"guard_passed": result["guard_passed"], "output": str(arguments.output)}))
                return 0
            raise HarnessError("unknown IN performance command")
    except common.PerformanceMismatch as error:
        print(f"IN performance mismatch: {error}", file=sys.stderr)
        return 2
    except HarnessError as error:
        print(f"IN performance harness error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
