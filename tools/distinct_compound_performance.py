#!/usr/bin/env python3

from __future__ import annotations

import argparse
import contextlib
import copy
import json
import pathlib
import sys
from typing import Any

if __package__:
    from tools import order_by_performance as base
    from tools import read_performance as common
else:
    import order_by_performance as base
    import read_performance as common


HarnessError = common.HarnessError
PerformanceMismatch = common.PerformanceMismatch
MINIMUM_WALL_NS = 5_000_000
GUARD_RATIO = 40
_COMMON_STATE = {
    "EXPECTED_CASE_IDS": common.EXPECTED_CASE_IDS,
    "MINIMUM_WALL_NS": common.MINIMUM_WALL_NS,
    "MAX_BASELINE_ARTIFACTS": common.MAX_BASELINE_ARTIFACTS,
    "ENFORCE_GUARD_ON_VALIDATION": common.ENFORCE_GUARD_ON_VALIDATION,
    "validate_workload_manifest": common.validate_workload_manifest,
    "_smoke_work": common._smoke_work,
    "_collect_input_references": common._collect_input_references,
    "validate_raw_timing_report": common.validate_raw_timing_report,
    "validate_raw_diagnostic_report": common.validate_raw_diagnostic_report,
    "_capture_actual_build_flags": common._capture_actual_build_flags,
}

CASE_IDS = (
    "distinct-low-card-memory",
    "distinct-high-card-memory",
    "distinct-collated-memory",
    "distinct-order-limit-memory",
    "values-union-all-limit-memory",
    "union-replace-file",
    "except-membership-memory",
    "intersect-membership-memory",
    "ordered-union-all-topn-memory",
    "ordered-union-merge-memory",
    "ordered-except-merge-memory",
    "ordered-intersect-merge-memory",
    "long-mixed-compound-memory",
    "union-keyed-relation-before-file",
)


def _large_values_sql() -> str:
    values = ",".join(f"({value})" for value in range(1, 257))
    return f"VALUES{values} UNION ALL SELECT id+0 FROM items ORDER BY 1 LIMIT 64"


CASE_CONTRACTS = {
    "distinct-low-card-memory": (
        "SELECT DISTINCT flag FROM items",
        2,
        "memory",
        64 << 20,
    ),
    "distinct-high-card-memory": (
        "SELECT DISTINCT category FROM items",
        65_536,
        "memory",
        64 << 20,
    ),
    "distinct-collated-memory": (
        "SELECT DISTINCT iif(flag=0,'A','a') COLLATE NOCASE FROM items",
        1,
        "memory",
        64 << 20,
    ),
    "distinct-order-limit-memory": (
        "SELECT DISTINCT score+0 FROM items ORDER BY 1 DESC LIMIT 64",
        64,
        "memory",
        64 << 20,
    ),
    "values-union-all-limit-memory": (
        _large_values_sql(),
        64,
        "memory",
        64 << 20,
    ),
    "union-replace-file": (
        "SELECT score FROM items WHERE id<=32768 "
        "UNION SELECT score+0.0 FROM items WHERE id>32768",
        4_096,
        "file",
        2 << 20,
    ),
    "except-membership-memory": (
        "SELECT score FROM items WHERE flag=0 "
        "EXCEPT SELECT score FROM items WHERE id%4=0",
        1_024,
        "memory",
        64 << 20,
    ),
    "intersect-membership-memory": (
        "SELECT score FROM items WHERE flag=0 "
        "INTERSECT SELECT score FROM items WHERE id%4=0",
        1_024,
        "memory",
        64 << 20,
    ),
    "ordered-union-all-topn-memory": (
        "SELECT id,payload FROM items WHERE flag=0 "
        "UNION ALL SELECT id,payload FROM items WHERE flag=1 "
        "UNION ALL SELECT id,payload FROM items WHERE id%4=0 "
        "ORDER BY payload,id LIMIT 64 OFFSET 64",
        64,
        "memory",
        64 << 20,
    ),
    "ordered-union-merge-memory": (
        "SELECT score FROM items WHERE id<=32768 "
        "UNION SELECT score+0.0 FROM items WHERE id>32768 ORDER BY 1",
        4_096,
        "memory",
        64 << 20,
    ),
    "ordered-except-merge-memory": (
        "SELECT score FROM items WHERE flag=0 "
        "EXCEPT SELECT score FROM items WHERE id%4=0 ORDER BY 1",
        1_024,
        "memory",
        64 << 20,
    ),
    "ordered-intersect-merge-memory": (
        "SELECT score FROM items WHERE flag=0 "
        "INTERSECT SELECT score FROM items WHERE id%4=0 ORDER BY 1",
        1_024,
        "memory",
        64 << 20,
    ),
    "long-mixed-compound-memory": (
        "SELECT score FROM items WHERE id<=16384 "
        "UNION ALL SELECT score FROM items WHERE id>16384 AND id<=32768 "
        "UNION SELECT score FROM items WHERE id>32768 AND id<=49152 "
        "EXCEPT SELECT score FROM items WHERE id%8=0 "
        "INTERSECT SELECT score FROM items WHERE flag=1 ORDER BY 1",
        2_048,
        "memory",
        64 << 20,
    ),
    "union-keyed-relation-before-file": (
        "SELECT category,payload FROM items WHERE id<=32768 "
        "UNION SELECT category,payload FROM items WHERE id>32768",
        65_536,
        "file",
        2 << 20,
    ),
}

CASE_ITEM_COUNTS = {
    "distinct-low-card-memory": 65_536,
    "distinct-high-card-memory": 65_536,
    "distinct-collated-memory": 65_536,
    "distinct-order-limit-memory": 65_536,
    "values-union-all-limit-memory": 65_792,
    "union-replace-file": 131_072,
    "except-membership-memory": 131_072,
    "intersect-membership-memory": 131_072,
    "ordered-union-all-topn-memory": 196_608,
    "ordered-union-merge-memory": 131_072,
    "ordered-except-merge-memory": 131_072,
    "ordered-intersect-merge-memory": 131_072,
    "long-mixed-compound-memory": 327_680,
    "union-keyed-relation-before-file": 131_072,
}

CASE_MEASURED_ITERATIONS = {
    case_id: (
        2
        if case_id
        in {
            "distinct-low-card-memory",
            "distinct-collated-memory",
            "distinct-order-limit-memory",
        }
        else 4
        if case_id == "values-union-all-limit-memory"
        else 1
    )
    for case_id in CASE_IDS
}

CASE_MEASURED_DIGESTS = {
    "distinct-low-card-memory": "a5d1922acdc6225d",
    "distinct-collated-memory": "2596634236bd4ec5",
    "distinct-order-limit-memory": "b9171f9835740255",
    "values-union-all-limit-memory": "e9f68d7c90b0b405",
}

SQLITE_FULLSCAN_STEPS = {
    "distinct-low-card-memory": 65_535,
    "distinct-high-card-memory": 65_535,
    "distinct-collated-memory": 65_535,
    "distinct-order-limit-memory": 65_535,
    "values-union-all-limit-memory": 65_535,
    "union-replace-file": 65_535,
    "except-membership-memory": 131_070,
    "intersect-membership-memory": 131_039,
    "ordered-union-all-topn-memory": 196_605,
    "ordered-union-merge-memory": 65_535,
    "ordered-except-merge-memory": 131_070,
    "ordered-intersect-merge-memory": 131_039,
    "long-mixed-compound-memory": 196_590,
    "union-keyed-relation-before-file": 0,
}

SQLITE_SORT_OPERATIONS = {
    "distinct-low-card-memory": 0,
    "distinct-high-card-memory": 0,
    "distinct-collated-memory": 0,
    "distinct-order-limit-memory": 1,
    "values-union-all-limit-memory": 2,
    "union-replace-file": 1,
    "except-membership-memory": 0,
    "intersect-membership-memory": 0,
    "ordered-union-all-topn-memory": 3,
    "ordered-union-merge-memory": 1,
    "ordered-except-merge-memory": 0,
    "ordered-intersect-merge-memory": 0,
    "long-mixed-compound-memory": 2,
    "union-keyed-relation-before-file": 2,
}

SQLITE_SPILL_CASES = {"union-keyed-relation-before-file"}


_BASE_CASE_IDS = base.CASE_IDS
_BASE_CASE_CONTRACTS = base._CASE_CONTRACTS
_BASE_MINIMUM_WALL_NS = base.MINIMUM_WALL_NS


@contextlib.contextmanager
def _configured_base() -> Any:
    base.CASE_IDS = CASE_IDS
    base._CASE_CONTRACTS = CASE_CONTRACTS
    base.MINIMUM_WALL_NS = MINIMUM_WALL_NS
    try:
        yield
    finally:
        base.CASE_IDS = _BASE_CASE_IDS
        base._CASE_CONTRACTS = _BASE_CASE_CONTRACTS
        base.MINIMUM_WALL_NS = _BASE_MINIMUM_WALL_NS


def validate_workload_manifest(
    value: Any,
    *,
    repository_root: pathlib.Path,
    manifest_path: pathlib.Path,
) -> dict[str, Any]:
    normalized = copy.deepcopy(value)
    if isinstance(normalized, dict):
        normalized["guard"] = {
            "maximum_cpu_ratio": {"numerator": 10, "denominator": 1},
            "maximum_wall_ratio": {"numerator": 10, "denominator": 1},
        }
    if isinstance(normalized, dict) and isinstance(normalized.get("cases"), list):
        for case in normalized["cases"]:
            if not isinstance(case, dict) or case.get("id") not in CASE_ITEM_COUNTS:
                continue
            case["items_per_iteration"] = base.ROW_COUNT
            case["measured_iterations"] = 1
            expected = case.get("expected")
            if not isinstance(expected, dict):
                continue
            for group, iterations in (
                ("warmup", case.get("warmup_iterations")),
                ("measured", case.get("measured_iterations")),
                ("diagnostic", case.get("diagnostic_iterations")),
                ("smoke", 1),
            ):
                work = expected.get(group)
                if isinstance(work, dict) and isinstance(iterations, int):
                    work["items"] = iterations * base.ROW_COUNT
            if isinstance(expected, dict) and isinstance(expected.get("smoke"), dict):
                expected["measured"] = copy.deepcopy(expected["smoke"])
    with _configured_base():
        base.validate_workload_manifest(
            normalized,
            repository_root=repository_root,
            manifest_path=manifest_path,
        )
    if not isinstance(value, dict) or not isinstance(value.get("cases"), list):
        raise HarnessError("DISTINCT/compound workload cases must be a list")
    expected_guard = {
        "maximum_cpu_ratio": {"numerator": GUARD_RATIO, "denominator": 1},
        "maximum_wall_ratio": {"numerator": GUARD_RATIO, "denominator": 1},
    }
    if value.get("guard") != expected_guard:
        raise HarnessError(
            f"DISTINCT/compound workload guard must be exactly {GUARD_RATIO}/1"
        )
    for index, case in enumerate(value["cases"]):
        if not isinstance(case, dict):
            raise HarnessError(
                f"DISTINCT/compound workload cases[{index}] must be an object"
            )
        case_id = case.get("id")
        if case_id not in CASE_ITEM_COUNTS:
            raise HarnessError("DISTINCT/compound workload contains an unknown case")
        expected_items = CASE_ITEM_COUNTS[case_id]
        expected_measured_iterations = CASE_MEASURED_ITERATIONS[case_id]
        if case.get("items_per_iteration") != expected_items:
            raise HarnessError(
                f"DISTINCT/compound workload cases[{index}].items_per_iteration "
                f"must be {expected_items}"
            )
        if case.get("measured_iterations") != expected_measured_iterations:
            raise HarnessError(
                f"DISTINCT/compound workload cases[{index}].measured_iterations "
                f"must be {expected_measured_iterations}"
            )
        expected = case.get("expected")
        if not isinstance(expected, dict):
            raise HarnessError(
                f"DISTINCT/compound workload cases[{index}].expected must be an object"
            )
        measured = expected.get("measured")
        common._validate_work(
            measured,
            f"DISTINCT/compound workload cases[{index}].expected.measured",
        )
        smoke = expected.get("smoke")
        common._validate_work(
            smoke,
            f"DISTINCT/compound workload cases[{index}].expected.smoke",
        )
        _, result_rows, _, _ = CASE_CONTRACTS[case_id]
        measured_digest = CASE_MEASURED_DIGESTS.get(case_id, smoke["digest"])
        expected_measured = {
            "operations": expected_measured_iterations,
            "items": expected_measured_iterations * expected_items,
            "rows": expected_measured_iterations * result_rows,
            "bytes": expected_measured_iterations * smoke["bytes"],
            "result_hits": expected_measured_iterations,
            "result_misses": 0,
            "digest": measured_digest,
        }
        if measured != expected_measured:
            raise HarnessError(
                f"DISTINCT/compound workload cases[{index}].expected.measured "
                "does not match its pinned work"
            )
        for group, iterations in (
            ("warmup", case.get("warmup_iterations")),
            ("measured", case.get("measured_iterations")),
            ("diagnostic", case.get("diagnostic_iterations")),
            ("smoke", 1),
        ):
            work = expected.get(group)
            if (
                not isinstance(work, dict)
                or not isinstance(iterations, int)
                or work.get("items") != iterations * expected_items
            ):
                raise HarnessError(
                    f"DISTINCT/compound workload cases[{index}].expected.{group} "
                    "does not match its source-row scale"
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
        return base._COMMON_VALIDATE_RAW_DIAGNOSTIC_REPORT(
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
            expected_names=base.ORDER_SQLITE_COUNTER_NAMES,
            label="diagnostic report.counters.sqlite",
        )
        selected.pop("sort_operations")
        selected.pop("temp_bytes_spilled")
        selected["cache_writes"] = 0
        selected["cache_misses"] = 0
        selected["fullscan_steps"] = 0
    else:
        raise HarnessError("expected engine must be modern or sqlite")
    base._COMMON_VALIDATE_RAW_DIAGNOSTIC_REPORT(
        normalized,
        workload_manifest=workload_manifest,
        expected_engine=expected_engine,
        expected_case=expected_case,
    )
    case = common._case_by_id(workload_manifest, expected_case)
    expected_configuration = base._expected_case_configuration(workload_manifest, case)
    if actual_configuration != expected_configuration:
        raise HarnessError(
            "diagnostic report.effective_configuration does not match the "
            "DISTINCT/compound case"
        )
    counters = value["counters"][expected_engine]
    if expected_engine == "modern":
        if counters["pages_read"] != counters["cache_misses"]:
            raise HarnessError(
                "Modern DISTINCT/compound diagnostic page reads and cache misses "
                "must match"
            )
    else:
        if counters["fullscan_steps"] != SQLITE_FULLSCAN_STEPS[expected_case]:
            raise HarnessError(
                "SQLite DISTINCT/compound full-scan count does not match the case"
            )
        if counters["sort_operations"] != SQLITE_SORT_OPERATIONS[expected_case]:
            raise HarnessError(
                "SQLite DISTINCT/compound sort count does not match the case"
            )
        spilled = counters["temp_bytes_spilled"] > 0
        if spilled != (expected_case in SQLITE_SPILL_CASES):
            raise HarnessError(
                "SQLite DISTINCT/compound spill classification does not match the case"
            )
    return value


def _collect_input_references(
    *,
    repository_root: pathlib.Path,
    workload_path: pathlib.Path,
    workload_manifest: dict[str, Any],
) -> list[dict[str, Any]]:
    profile_path = common._resolve_repository_file(
        repository_root,
        workload_manifest["sqlite_profile"],
        "DISTINCT/compound workload sqlite_profile",
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
            path=repository_root / "benchmarks/distinct_compound_performance.cpp",
            repository_root=repository_root,
        ),
        common._repository_input_reference(
            role="shared-benchmark-source",
            path=repository_root / "benchmarks/order_by_performance.cpp",
            repository_root=repository_root,
        ),
        common._repository_input_reference(
            role="shared-runner-source",
            path=repository_root / "tools/read_performance.py",
            repository_root=repository_root,
        ),
        common._repository_input_reference(
            role="shared-order-by-runner-source",
            path=repository_root / "tools/order_by_performance.py",
            repository_root=repository_root,
        ),
        common._repository_input_reference(
            role="distinct-compound-runner-source",
            path=repository_root / "tools/distinct_compound_performance.py",
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
            "CMakeFiles/modern_sqlite_distinct_compound_benchmark.dir/",
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
    target = "modern_sqlite_distinct_compound_benchmark"
    commands = common._command_output(
        [make_program, "-C", str(build_directory), "-t", "commands", target],
        cwd=repository_root,
        label="Ninja DISTINCT/compound timing-target command lookup",
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
            "Ninja command graph must contain one DISTINCT/compound timing "
            "executable link command"
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
    with _configured_base():
        base._configure_common()
    common.EXPECTED_CASE_IDS = CASE_IDS
    common.MINIMUM_WALL_NS = MINIMUM_WALL_NS
    common.MAX_BASELINE_ARTIFACTS = 300
    common.ENFORCE_GUARD_ON_VALIDATION = True
    common.validate_workload_manifest = validate_workload_manifest
    common._smoke_work = base._smoke_work
    common._collect_input_references = _collect_input_references
    common.validate_raw_timing_report = base.validate_raw_timing_report
    common.validate_raw_diagnostic_report = validate_raw_diagnostic_report
    common._capture_actual_build_flags = _capture_actual_build_flags


def _restore_common() -> None:
    for name, value in _COMMON_STATE.items():
        setattr(common, name, value)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_workloads = subparsers.add_parser("validate-workloads")
    validate_workloads.add_argument(
        "--repository-root", type=pathlib.Path, required=True
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
        print(f"DISTINCT/compound performance harness error: {error}", file=sys.stderr)
        return 1
    except PerformanceMismatch as error:
        print(f"DISTINCT/compound performance mismatch: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
