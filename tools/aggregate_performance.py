#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import copy
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
PerformanceMismatch = common.PerformanceMismatch
MINIMUM_WALL_NS = 5_000_000
GUARD_RATIO = 40

CASE_DATA = (
    ("global-functions-memory", "SELECT count(id+0),sum(score),total(score),avg(score),min(score+0),max(score+0) FROM items", 65536, 8, 1, 48, "4faaa8e066a13c6e", "92ddae023fd3037d", (65536, 1), (6, 393216, 6, 0, 6)),
    ("global-empty-memory", "SELECT count(id+0),sum(score),total(score),avg(score),min(score+0),max(score+0) FROM items WHERE 0", 0, 65536, 1, 16, "7d7eeab76b309583", "e2f49f8851122325", (0, 1), (6, 0, 6, 0, 6)),
    ("indexed-min-memory", "SELECT min(score) FROM items", 1, 65536, 1, 8, "156194db51eeea60", "946bff8b56ec2325", (1,), (1, 1, 1, 0, 1)),
    ("indexed-max-memory", "SELECT max(score) FROM items", 1, 65536, 1, 8, "e9a131d7326b603a", "e9268781575a2325", (1,), (1, 1, 1, 0, 1)),
    ("ordered-group-memory", "SELECT score,count(id) FROM items GROUP BY score ORDER BY score", 65536, 16, 4096, 65536, "c8343924ddebd3ee", "135eb317488e5ca5", (65536, 4096), (1, 65536, 4096, 4095, 1)),
    ("sorted-group-memory", "SELECT score+0,count(id) FROM items GROUP BY score+0 ORDER BY 1", 65536, 4, 4096, 65536, "c8343924ddebd3ee", "ddb07cac1b8f3485", (65536, 4096), (1, 65536, 4096, 4095, 1)),
    ("sorted-group-file", "SELECT score+0,count(id) FROM items GROUP BY score+0 ORDER BY 1", 65536, 8, 4096, 65536, "c8343924ddebd3ee", "7b09094f028c0ce5", (65536, 4096), (1, 65536, 4096, 4095, 1)),
    ("high-card-group-memory", "SELECT category,count(id) FROM items GROUP BY category ORDER BY category", 65536, 2, 65536, 1638400, "4bcfdb1cb4de1e15", "957da2d4a8db7f45", (65536, 65536), (1, 65536, 65536, 65535, 1)),
    ("grouped-having-topn-memory", "SELECT score+0 AS g,count(id) AS n,sum(id) AS s FROM items GROUP BY score+0 HAVING count(id)>8 ORDER BY s DESC,g LIMIT 64 OFFSET 64", 65536, 4, 64, 1536, "0e03c4d112c9b3dc", "5a77b6fb3107ee95", (65536, 65536), (2, 131072, 8192, 8190, 2)),
    ("distinct-state-memory", "SELECT count(DISTINCT score),sum(DISTINCT score) FROM items", 65536, 4, 1, 16, "9831a884d4b5c935", "e9671c75b1ed1c25", (65536, 65536), (2, 8192, 2, 0, 2)),
    ("filtered-state-memory", "SELECT count(id) FILTER (WHERE flag=1),sum(score) FILTER (WHERE flag=0) FROM items", 65536, 8, 1, 16, "69b488f754f8f77e", "d781388b7c8cf59d", (32768, 32768), (2, 65536, 2, 0, 2)),
    ("local-order-memory", "SELECT group_concat(score ORDER BY score DESC,id) FROM items", 65536, 4, 1, 309919, "5d8975a11dedb5b6", "3d2df2add6d5ca81", (65536,), (1, 65536, 1, 0, 1)),
    ("unique-local-order-memory", "SELECT group_concat(DISTINCT score ORDER BY score) FROM items", 65536, 8, 1, 19369, "09579402da892a6b", "00cd5e2543ad11d5", (65536,), (1, 4096, 1, 0, 1)),
    ("wide-modifier-file", "SELECT group_concat(score ORDER BY id),group_concat(score ORDER BY id DESC),count(DISTINCT category),group_concat(DISTINCT score ORDER BY score) FROM items", 65536, 1, 1, 639215, "c9abc9b4135a16a2", "c9abc9b4135a16a2", (65536, 65536, 65536, 65536), (4, 200704, 4, 0, 4)),
    ("aggregate-compound-memory", "SELECT score+0 AS g,count(id) FROM items GROUP BY score+0 UNION ALL SELECT -1,count(id) FROM items ORDER BY 1 LIMIT 64 OFFSET 64", 131072, 4, 64, 1024, "ddf54a75aaf0af2e", "0431523d9bf32905", (65536, 65536), (2, 131072, 4097, 4095, 2)),
)
CASE_IDS = tuple(case[0] for case in CASE_DATA)
CASE_BY_ID = {case[0]: case for case in CASE_DATA}
FILE_CASES = frozenset(("sorted-group-file", "wide-modifier-file"))
OPERATION_NAMES = ("open", "step", "finalize", "reset", "close")
PEAK_NAMES = (
    "retained_bytes_peak", "working_set_bytes_peak", "live_files_peak",
    "file_acquisitions", "sorter_spill_writes",
)
MECHANISMS = {
    "global-functions-memory": (65535, 0, False),
    "global-empty-memory": (0, 0, False),
    "indexed-min-memory": (0, 0, False),
    "indexed-max-memory": (0, 0, False),
    "ordered-group-memory": (65535, 0, False),
    "sorted-group-memory": (65535, 1, True),
    "sorted-group-file": (65535, 1, True),
    "high-card-group-memory": (65535, 1, True),
    "grouped-having-topn-memory": (65535, 2, True),
    "distinct-state-memory": (65535, 0, True),
    "filtered-state-memory": (65535, 0, False),
    "local-order-memory": (65535, 0, True),
    "unique-local-order-memory": (65535, 0, True),
    "wide-modifier-file": (65535, 0, True),
    "aggregate-compound-memory": (131070, 2, True),
}
_COMMON_STATE = {
    name: getattr(common, name)
    for name in (
        "EXPECTED_CASE_IDS", "MINIMUM_WALL_NS", "MAX_BASELINE_ARTIFACTS",
        "ENFORCE_GUARD_ON_VALIDATION", "validate_workload_manifest", "_smoke_work",
        "_collect_input_references", "validate_raw_timing_report",
        "validate_raw_diagnostic_report", "_capture_actual_build_flags",
    )
}


def expected_probe_counts(case_id: str) -> list[int]:
    return list(CASE_BY_ID[case_id][8])


def expected_operations(case_id: str) -> dict[str, int]:
    return dict(zip(OPERATION_NAMES, CASE_BY_ID[case_id][9]))


def _work(case: tuple[Any, ...], measured: bool = False) -> dict[str, Any]:
    iterations = case[3] if measured else 1
    return {
        "operations": iterations,
        "items": iterations * case[2],
        "rows": iterations * case[4],
        "bytes": iterations * case[5],
        "result_hits": iterations,
        "result_misses": 0,
        "digest": case[7] if measured else case[6],
    }


@contextlib.contextmanager
def _configured_order_contract() -> Any:
    previous = (base.CASE_IDS, base._CASE_CONTRACTS, base.MINIMUM_WALL_NS)
    base.CASE_IDS = CASE_IDS
    base._CASE_CONTRACTS = {
        case[0]: (case[1], case[4], "file" if case[0] in FILE_CASES else "memory",
                  2 << 20 if case[0] in FILE_CASES else 64 << 20)
        for case in CASE_DATA
    }
    base.MINIMUM_WALL_NS = MINIMUM_WALL_NS
    try:
        yield
    finally:
        base.CASE_IDS, base._CASE_CONTRACTS, base.MINIMUM_WALL_NS = previous


def validate_workload_manifest(
    value: Any, *, repository_root: pathlib.Path, manifest_path: pathlib.Path
) -> dict[str, Any]:
    common._require_type(value, dict, "aggregate workload manifest")
    common._require_exact_keys(value, base._TOP_KEYS, "aggregate workload manifest")
    contracts._validate_original_manifest_types(value)
    common._require_type(value["cases"], list, "aggregate workload cases")
    if len(value["cases"]) != len(CASE_DATA):
        raise HarnessError("aggregate workload case IDs are not pinned")
    contracts._require_exact_typed(
        value["guard"],
        {"maximum_cpu_ratio": {"numerator": 40, "denominator": 1},
         "maximum_wall_ratio": {"numerator": 40, "denominator": 1}},
        "aggregate workload guard",
    )
    for actual, case in zip(value["cases"], CASE_DATA):
        label = f"aggregate workload {case[0]}"
        common._require_type(actual, dict, label)
        common._require_exact_keys(
            actual, base._CASE_KEYS | {"diagnostic_probe_groups", "aggregate_operations"}, label
        )
        expected = {
            "id": case[0], "fixture": base.FIXTURE_ID, "kind": "order-by", "sql": case[1],
            "primary_unit": "source-row", "warmup_iterations": 1,
            "measured_iterations": case[3], "diagnostic_iterations": 1,
            "items_per_iteration": case[2], "result_rows_per_iteration": case[4],
            "query_only": True, "temporary_store": "file" if case[0] in FILE_CASES else "memory",
            "sorter_memory_threshold": 2 << 20 if case[0] in FILE_CASES else 64 << 20,
            "diagnostic_probe_groups": [
                {"tag_count": 1, "calls_per_tag": count} for count in case[8]
            ],
            "aggregate_operations": expected_operations(case[0]),
            "expected": {
                "warmup": _work(case), "measured": _work(case, True), "diagnostic": _work(case),
                "smoke": _work(case), "verification": contracts.VERIFICATION_WORK,
            },
        }
        contracts._require_exact_typed(actual, expected, label)
    normalized = copy.deepcopy(value)
    normalized["guard"] = {
        "maximum_cpu_ratio": {"numerator": 10, "denominator": 1},
        "maximum_wall_ratio": {"numerator": 10, "denominator": 1},
    }
    for case in normalized["cases"]:
        case.pop("diagnostic_probe_groups")
        case.pop("aggregate_operations")
        case["items_per_iteration"] = base.ROW_COUNT
        case["measured_iterations"] = 1
        for phase in ("warmup", "measured", "diagnostic", "smoke"):
            case["expected"][phase] = dict(case["expected"]["smoke"])
            case["expected"][phase]["items"] = base.ROW_COUNT
    with _configured_order_contract():
        base.validate_workload_manifest(
            normalized, repository_root=repository_root, manifest_path=manifest_path
        )
    contracts._require_exact_typed(value["schema_version"], 1, "aggregate schema version")
    contracts._require_exact_typed(
        value["workload_semantics_version"], 1, "aggregate workload version"
    )
    contracts._require_exact_typed(value["minimum_wall_ns"], MINIMUM_WALL_NS, "aggregate minimum wall")
    return value


def validate_raw_timing_report(
    value: Any, *, workload_manifest: dict[str, Any], expected_engine: str,
    expected_case: str, expected_run_kind: str = "baseline",
) -> dict[str, Any]:
    common._require_type(value, dict, "aggregate timing report")
    common._require_exact_keys(value, common._RAW_TIMING_KEYS, "aggregate timing report")
    contracts._validate_report_versions(
        value, ("schema_version", "completion_schema_version", "workload_semantics_version"),
        "aggregate timing report",
    )
    case = common._case_by_id(workload_manifest, expected_case)
    contracts._require_exact_typed(
        value["effective_configuration"], base._expected_case_configuration(workload_manifest, case),
        "aggregate effective configuration",
    )
    return base.validate_raw_timing_report(
        value, workload_manifest=workload_manifest, expected_engine=expected_engine,
        expected_case=expected_case, expected_run_kind=expected_run_kind,
    )


def validate_raw_diagnostic_report(
    value: Any, *, workload_manifest: dict[str, Any], expected_engine: str, expected_case: str,
) -> dict[str, Any]:
    common._require_type(value, dict, "aggregate diagnostic report")
    common._require_exact_keys(
        value, set(common._RAW_DIAGNOSTIC_KEYS) | {"probe_counts", "aggregate_work"},
        "aggregate diagnostic report",
    )
    contracts._validate_report_versions(
        value, ("schema_version", "completion_schema_version", "diagnostic_schema_version",
                "workload_semantics_version"), "aggregate diagnostic report",
    )
    contracts._require_exact_typed(
        value["probe_counts"], expected_probe_counts(expected_case), "aggregate expression probes"
    )
    common._require_type(value["aggregate_work"], dict, "aggregate work diagnostics")
    if expected_engine == "modern":
        common._require_exact_keys(
            value["aggregate_work"], set(OPERATION_NAMES) | set(PEAK_NAMES), "aggregate work diagnostics"
        )
        for name, expected in expected_operations(expected_case).items():
            contracts._require_exact_typed(value["aggregate_work"][name], expected, f"aggregate {name}")
        for name in PEAK_NAMES:
            common._require_integer(value["aggregate_work"][name], f"aggregate {name}")
        if value["aggregate_work"]["live_files_peak"] > 2:
            raise HarnessError("aggregate live file ceiling exceeded")
        case = common._case_by_id(workload_manifest, expected_case)
        if value["aggregate_work"]["retained_bytes_peak"] > case["sorter_memory_threshold"]:
            raise HarnessError("aggregate retained-memory admission budget exceeded")
        if expected_case not in FILE_CASES and any(
            value["aggregate_work"][name] != 0
            for name in ("working_set_bytes_peak", "live_files_peak", "file_acquisitions", "sorter_spill_writes")
        ):
            raise HarnessError("aggregate memory case acquired file resources")
        if expected_case in FILE_CASES and value["aggregate_work"]["file_acquisitions"] == 0:
            raise HarnessError("aggregate file-pressure case did not acquire a spill file")
        if expected_case in FILE_CASES and value["aggregate_work"]["live_files_peak"] == 0:
            raise HarnessError("aggregate file-pressure case has no live file peak")
        if (value["aggregate_work"]["retained_bytes_peak"] > 0) != MECHANISMS[expected_case][2]:
            raise HarnessError("aggregate retained-storage mechanism differs from the contract")
    elif expected_engine == "sqlite":
        contracts._require_exact_typed(value["aggregate_work"], {}, "SQLite aggregate callback metrics")
    else:
        raise HarnessError("aggregate engine must be modern or sqlite")
    case = common._case_by_id(workload_manifest, expected_case)
    contracts._require_exact_typed(
        {key: value[key] for key in ("mode", "engine", "case")},
        {"mode": "diagnostic", "engine": expected_engine, "case": expected_case},
        "aggregate diagnostic identity",
    )
    common._validate_build(value["build"], instrumentation=True, label="aggregate diagnostic build")
    common._validate_sqlite(
        value["sqlite"], expected_semantic_options=workload_manifest["sqlite_semantic_compile_options"],
        label="aggregate diagnostic SQLite identity",
    )
    common._require_type(value["counters"], dict, "aggregate diagnostic counters")
    common._require_exact_keys(
        value["counters"], common._COUNTER_GROUP_KEYS, "aggregate diagnostic counter engines"
    )
    contracts._require_exact_typed(
        value["counters"]["sqlite" if expected_engine == "modern" else "modern"],
        {}, "inactive aggregate counter engine",
    )
    selected = value["counters"][expected_engine]
    if expected_engine == "modern":
        common._validate_counter_object(
            selected, expected_names=common.MODERN_COUNTER_NAMES, label="aggregate Modern counters"
        )
        if selected["pages_read"] != selected["cache_misses"]:
            raise HarnessError("aggregate page reads and cache misses must agree")
        if any(selected[name] == 0 for name in ("allocations", "vm_instructions", "planner_work")):
            raise HarnessError("aggregate Modern execution counters must be nonzero")
        if case["items_per_iteration"] > 0 and any(
            selected[name] == 0 for name in ("vfs_calls", "cache_hits")
        ):
            raise HarnessError("aggregate nonempty Modern source requires observed I/O")
        if case["items_per_iteration"] == 0 and any(
            selected[name] != 0 for name in ("pages_read", "pages_written", "cache_hits", "btree_comparisons")
        ):
            raise HarnessError("aggregate empty source unexpectedly accessed data pages")
        if expected_case in FILE_CASES and value["aggregate_work"]["sorter_spill_writes"] == 0:
            raise HarnessError("aggregate file-pressure mechanism did not write PMA records")
        if expected_case not in FILE_CASES and selected["pages_written"] != 0:
            raise HarnessError("aggregate memory mechanism unexpectedly wrote pages")
        if expected_case in ("indexed-min-memory", "indexed-max-memory") and (
            selected["vm_instructions"] > 20 or selected["cache_hits"] > 4
        ):
            raise HarnessError("aggregate indexed source-access bound was exceeded")
    else:
        common._validate_counter_object(
            selected, expected_names=base.ORDER_SQLITE_COUNTER_NAMES, label="aggregate SQLite counters"
        )
        if any(selected[name] == 0 for name in ("vm_steps", "statement_runs", "cache_bytes_current")):
            raise HarnessError("aggregate SQLite execution counters must be nonzero")
        fullscan, sorts, _ = MECHANISMS[expected_case]
        contracts._require_exact_typed(
            {"fullscan_steps": selected["fullscan_steps"], "sort_operations": selected["sort_operations"]},
            {"fullscan_steps": fullscan, "sort_operations": sorts},
            "aggregate native source/sort mechanism",
        )
        if selected["temp_bytes_spilled"] != 0:
            raise HarnessError("aggregate native TEMPBUF_SPILL classification changed")
        if expected_case in ("indexed-min-memory", "indexed-max-memory") and (
            selected["vm_steps"] > 20 or selected["cache_hits"] > 4
        ):
            raise HarnessError("aggregate native indexed source-access bound was exceeded")
        if expected_case not in FILE_CASES and (
            selected["cache_writes"] != 0 or selected["temp_bytes_spilled"] != 0
        ):
            raise HarnessError("aggregate SQLite memory case unexpectedly spilled")
    common._validate_expected_work(
        value["work"], case["expected"]["diagnostic"], "aggregate diagnostic fixed work"
    )
    common._validate_completion(
        value["completion"], "aggregate diagnostic completion",
        common._expected_completion(case, mode="diagnostic"),
    )
    contracts._require_exact_typed(
        value["effective_configuration"], base._expected_case_configuration(workload_manifest, case),
        "aggregate diagnostic configuration",
    )
    return value


def _collect_input_references(
    *, repository_root: pathlib.Path, workload_path: pathlib.Path, workload_manifest: dict[str, Any],
) -> list[dict[str, Any]]:
    sources = (
        ("workload-manifest", workload_path),
        ("sqlite-profile", repository_root / workload_manifest["sqlite_profile"]),
        ("aggregate-benchmark-source", repository_root / "benchmarks/aggregate_performance.cpp"),
        ("shared-benchmark-source", repository_root / "benchmarks/order_by_performance.cpp"),
        ("aggregate-runner-source", repository_root / "tools/aggregate_performance.py"),
        ("shared-runner-source", repository_root / "tools/read_performance.py"),
        ("shared-order-runner-source", repository_root / "tools/order_by_performance.py"),
        ("shared-contract-source", repository_root / "tools/distinct_compound_performance.py"),
    )
    references = [
        common._repository_input_reference(role=role, path=path, repository_root=repository_root)
        for role, path in sources
    ]
    for fixture in workload_manifest["fixtures"]:
        for suffix, field in (("", "path"), ("-sql", "sql_path")):
            references.append(common._repository_input_reference(
                role=f"fixture-{fixture['id']}{suffix}",
                path=repository_root / fixture[field], repository_root=repository_root,
            ))
    return references


def _capture_actual_build_flags(
    *, cache: dict[str, str], repository_root: pathlib.Path, build_directory: pathlib.Path,
    sqlite_source_directory: pathlib.Path,
) -> dict[str, str]:
    if cache["CMAKE_GENERATOR"] != "Ninja":
        raise HarnessError("aggregate provenance requires the pinned Ninja generator")
    target = "modern_sqlite_aggregate_benchmark"
    commands = common.load_json_strict(build_directory / "compile_commands.json")
    common._require_type(commands, list, "aggregate compile commands")
    targets = {
        "actual_sqlite_c_compile_flags": "CMakeFiles/modern_sqlite_benchmark_sqlite.dir/",
        "actual_modern_cxx_compile_flags": "CMakeFiles/modern_sqlite.dir/",
        "actual_harness_cxx_compile_flags": f"CMakeFiles/{target}.dir/",
    }
    captured = {}
    for field, needle in targets.items():
        flags = set()
        for command in commands:
            common._require_type(command, dict, "aggregate compile command")
            text = common._require_string(command.get("command"), "aggregate compile command text")
            if needle not in text:
                continue
            actual = common._extract_compile_flags(
                text, repository_root=repository_root, build_directory=build_directory,
                sqlite_source_directory=sqlite_source_directory,
            )
            common._validate_benchmark_flags(actual, require_optimization=True, label=field)
            flags.add(actual)
        if not flags:
            raise HarnessError(f"aggregate actual compiler flags missing: {field}")
        captured[field] = "\n".join(sorted(flags))
    make_program = cache.get("CMAKE_MAKE_PROGRAM")
    if not make_program:
        raise HarnessError("aggregate CMake cache is missing the make program")
    graph = common._command_output(
        [make_program, "-C", str(build_directory), "-t", "commands", target],
        cwd=repository_root, label="aggregate timing link lookup",
    ).decode("utf-8", errors="strict")
    links = [
        line for line in graph.splitlines()
        if target in line and " -c " not in line
        and (f" -o {target} " in line or f"/OUT:{target}".upper() in line.upper())
    ]
    if len(links) != 1:
        raise HarnessError("aggregate timing target must have one link command")
    flags = common._extract_link_flags(
        links[0], compiler_path=cache["CMAKE_CXX_COMPILER"], output_name=target,
        repository_root=repository_root, build_directory=build_directory,
        sqlite_source_directory=sqlite_source_directory,
    )
    common._validate_benchmark_flags(flags, require_optimization=False, label="aggregate link flags")
    captured["actual_timing_link_flags"] = flags
    return captured


def _configure_common() -> None:
    common.EXPECTED_CASE_IDS = CASE_IDS
    common.MINIMUM_WALL_NS = MINIMUM_WALL_NS
    common.MAX_BASELINE_ARTIFACTS = 300
    common.ENFORCE_GUARD_ON_VALIDATION = True
    common.validate_workload_manifest = validate_workload_manifest
    common._smoke_work = base._smoke_work
    common._collect_input_references = _collect_input_references
    common.validate_raw_timing_report = validate_raw_timing_report
    common.validate_raw_diagnostic_report = validate_raw_diagnostic_report
    common._capture_actual_build_flags = _capture_actual_build_flags


def _restore_common() -> None:
    for name, value in _COMMON_STATE.items():
        setattr(common, name, value)


def main(argv: list[str] | None = None) -> int:
    arguments = contracts._build_parser().parse_args(argv)
    try:
        _configure_common()
        if arguments.command == "validate-workloads":
            validate_workload_manifest(
                common.load_json_strict(arguments.workloads),
                repository_root=arguments.repository_root, manifest_path=arguments.workloads,
            )
            return 0
        if arguments.command == "validate-baseline":
            common.validate_baseline_directory(
                baseline_path=arguments.baseline, repository_root=arguments.repository_root,
                workload_path=arguments.workloads,
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
        raise HarnessError("unknown aggregate performance command")
    except PerformanceMismatch as error:
        print(f"aggregate performance mismatch: {error}", file=sys.stderr)
        return 2
    except HarnessError as error:
        print(f"aggregate performance harness error: {error}", file=sys.stderr)
        return 1
    finally:
        _restore_common()


if __name__ == "__main__":
    raise SystemExit(main())
