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
    "union-replace-file": 65_536,
    "except-membership-memory": 49_152,
    "intersect-membership-memory": 49_152,
    "ordered-union-all-topn-memory": 81_920,
    "ordered-union-merge-memory": 65_536,
    "ordered-except-merge-memory": 49_152,
    "ordered-intersect-merge-memory": 49_152,
    "long-mixed-compound-memory": 90_112,
    "union-keyed-relation-before-file": 65_536,
}

CASE_PROBE_GROUPS = {
    "distinct-low-card-memory": ((1, 65_536),),
    "distinct-high-card-memory": ((1, 65_536),),
    "distinct-collated-memory": ((1, 65_536),),
    "distinct-order-limit-memory": ((1, 65_536),),
    "values-union-all-limit-memory": ((256, 1), (1, 65_536)),
    "union-replace-file": ((2, 32_768),),
    "except-membership-memory": ((1, 32_768), (1, 16_384)),
    "intersect-membership-memory": ((1, 32_768), (1, 16_384)),
    "ordered-union-all-topn-memory": ((2, 32_768), (1, 16_384)),
    "ordered-union-merge-memory": ((2, 32_768),),
    "ordered-except-merge-memory": ((1, 32_768), (1, 16_384)),
    "ordered-intersect-merge-memory": ((1, 32_768), (1, 16_384)),
    "long-mixed-compound-memory": (
        (3, 16_384),
        (1, 8_192),
        (1, 32_768),
    ),
    "union-keyed-relation-before-file": ((2, 32_768),),
}


def expected_probe_counts(case_id: str) -> list[int]:
    return [
        calls
        for tag_count, calls in CASE_PROBE_GROUPS[case_id]
        for _ in range(tag_count)
    ]


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

CASE_SINGLE_RESULTS = {
    "distinct-low-card-memory": (2, 16, "e1e57e17779545b9"),
    "distinct-high-card-memory": (65_536, 1_114_112, "1115d744f5a9f725"),
    "distinct-collated-memory": (1, 1, "200b1b815c9ea247"),
    "distinct-order-limit-memory": (64, 512, "b23d465428610b6e"),
    "values-union-all-limit-memory": (64, 512, "ebfb42850728072e"),
    "union-replace-file": (4_096, 32_768, "f2f777673354d459"),
    "except-membership-memory": (1_024, 8_192, "7b9f56fa8bda78ee"),
    "intersect-membership-memory": (1_024, 8_192, "83509a56b1476dee"),
    "ordered-union-all-topn-memory": (64, 16_896, "9a886719eea2be2f"),
    "ordered-union-merge-memory": (4_096, 32_768, "f2f777673354d459"),
    "ordered-except-merge-memory": (1_024, 8_192, "7b9f56fa8bda78ee"),
    "ordered-intersect-merge-memory": (1_024, 8_192, "83509a56b1476dee"),
    "long-mixed-compound-memory": (2_048, 16_384, "aa64fc0c7c66e5ee"),
    "union-keyed-relation-before-file": (
        65_536,
        17_891_328,
        "ce76fdee5eb31ac8",
    ),
}

VERIFICATION_WORK = {
    "operations": 1,
    "items": 65_536,
    "rows": 65_536,
    "bytes": 17_301_504,
    "result_hits": 1,
    "result_misses": 0,
    "digest": "b760b119722fedf5",
}

SQLITE_SPILL_CASES = {"union-keyed-relation-before-file"}
MODERN_SPILL_CASES = {"union-keyed-relation-before-file"}


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


def _require_exact_typed(value: Any, expected: Any, label: str) -> None:
    if type(value) is not type(expected):
        raise HarnessError(
            f"{label} must be {type(expected).__name__}, got {type(value).__name__}"
        )
    if isinstance(expected, dict):
        common._require_exact_keys(value, set(expected), label)
        for key, expected_item in expected.items():
            _require_exact_typed(value[key], expected_item, f"{label}.{key}")
        return
    if isinstance(expected, list):
        if len(value) != len(expected):
            raise HarnessError(f"{label} length is not pinned")
        for index, expected_item in enumerate(expected):
            _require_exact_typed(value[index], expected_item, f"{label}[{index}]")
        return
    if value != expected:
        raise HarnessError(f"{label} does not match the pinned value")


def _validate_original_manifest_types(value: Any) -> None:
    base._require_keys(value, base._TOP_KEYS, "DISTINCT/compound workload manifest")
    common._require_integer(
        value["schema_version"],
        "DISTINCT/compound workload schema_version",
        minimum=1,
    )
    common._require_integer(
        value["workload_semantics_version"],
        "DISTINCT/compound workload workload_semantics_version",
        minimum=1,
    )
    common._require_string(
        value["sqlite_profile"],
        "DISTINCT/compound workload sqlite_profile",
    )
    compile_options = value["sqlite_semantic_compile_options"]
    common._require_type(
        compile_options,
        list,
        "DISTINCT/compound workload sqlite_semantic_compile_options",
    )
    for index, option in enumerate(compile_options):
        common._require_string(
            option,
            f"DISTINCT/compound workload sqlite_semantic_compile_options[{index}]",
        )
    _require_exact_typed(
        value["configuration"],
        {
            "page_size": 4096,
            "cache_pages": 512,
            "mmap_bytes": 0,
            "temp_store": "case",
            "sorter_memory_threshold": "case",
            "merge_fan_in": 16,
            "synchronous": "full",
            "journal_mode": "delete",
            "query_only": True,
            "thread_mode": "single",
        },
        "DISTINCT/compound workload configuration",
    )
    common._require_integer(
        value["minimum_wall_ns"],
        "DISTINCT/compound workload minimum_wall_ns",
        minimum=1,
    )
    _require_exact_typed(
        value["permutation"],
        {
            "algorithm": "splitmix64-rejection-fisher-yates-v1",
            "fingerprint": "fnv1a64-v1",
            "fit_seed": "9e3779b97f4a7c15",
            "pressure_seed": "d1b54a32d192ed03",
        },
        "DISTINCT/compound workload permutation",
    )
    _require_exact_typed(
        value["rounds"],
        [
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
        ],
        "DISTINCT/compound workload rounds",
    )
    fixtures = value["fixtures"]
    common._require_type(fixtures, list, "DISTINCT/compound workload fixtures")
    for index, fixture in enumerate(fixtures):
        label = f"DISTINCT/compound workload fixtures[{index}]"
        base._require_keys(fixture, base._FIXTURE_KEYS, label)
        for key in ("id", "path", "sql_path", "sha256", "sql_sha256"):
            common._require_string(fixture[key], f"{label}.{key}")
        for key in (
            "size_bytes",
            "page_size",
            "page_count",
            "row_count",
            "payload_size",
        ):
            common._require_integer(fixture[key], f"{label}.{key}", minimum=1)
    _require_exact_typed(
        value["guard"],
        {
            "maximum_cpu_ratio": {
                "numerator": GUARD_RATIO,
                "denominator": 1,
            },
            "maximum_wall_ratio": {
                "numerator": GUARD_RATIO,
                "denominator": 1,
            },
        },
        "DISTINCT/compound workload guard",
    )


def validate_workload_manifest(
    value: Any,
    *,
    repository_root: pathlib.Path,
    manifest_path: pathlib.Path,
) -> dict[str, Any]:
    _validate_original_manifest_types(value)
    common._require_type(
        value["cases"],
        list,
        "DISTINCT/compound workload cases",
    )
    expected_guard = {
        "maximum_cpu_ratio": {"numerator": GUARD_RATIO, "denominator": 1},
        "maximum_wall_ratio": {"numerator": GUARD_RATIO, "denominator": 1},
    }
    if value.get("guard") != expected_guard:
        raise HarnessError(
            f"DISTINCT/compound workload guard must be exactly {GUARD_RATIO}/1"
        )
    for index, case in enumerate(value["cases"]):
        label = f"DISTINCT/compound workload cases[{index}]"
        common._require_type(case, dict, label)
        common._require_exact_keys(
            case,
            base._CASE_KEYS | {"diagnostic_probe_groups"},
            label,
        )
        case_id = common._require_identifier(
            case.get("id"),
            f"{label}.id",
        )
        if case_id not in CASE_ITEM_COUNTS:
            raise HarnessError("DISTINCT/compound workload contains an unknown case")
        expected_items = CASE_ITEM_COUNTS[case_id]
        expected_measured_iterations = CASE_MEASURED_ITERATIONS[case_id]
        sql, result_rows, temporary_store, threshold = CASE_CONTRACTS[case_id]
        _require_exact_typed(
            {
                key: case.get(key)
                for key in (
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
                )
            },
            {
                "fixture": base.FIXTURE_ID,
                "kind": "order-by",
                "sql": sql,
                "primary_unit": "source-row",
                "warmup_iterations": 1,
                "measured_iterations": expected_measured_iterations,
                "diagnostic_iterations": 1,
                "items_per_iteration": expected_items,
                "result_rows_per_iteration": result_rows,
                "query_only": True,
                "temporary_store": temporary_store,
                "sorter_memory_threshold": threshold,
            },
            f"DISTINCT/compound workload cases[{index}] contract",
        )
        expected_probe_groups = [
            {"tag_count": tag_count, "calls_per_tag": calls}
            for tag_count, calls in CASE_PROBE_GROUPS[case_id]
        ]
        probe_groups = case.get("diagnostic_probe_groups")
        common._require_type(
            probe_groups,
            list,
            f"DISTINCT/compound workload cases[{index}].diagnostic_probe_groups",
        )
        for group_index, group in enumerate(probe_groups):
            label = (
                f"DISTINCT/compound workload cases[{index}]"
                f".diagnostic_probe_groups[{group_index}]"
            )
            common._require_type(group, dict, label)
            common._require_exact_keys(
                group,
                {"tag_count", "calls_per_tag"},
                label,
            )
            common._require_integer(
                group["tag_count"],
                f"{label}.tag_count",
                minimum=1,
            )
            common._require_integer(
                group["calls_per_tag"],
                f"{label}.calls_per_tag",
                minimum=1,
            )
        if probe_groups != expected_probe_groups:
            raise HarnessError(
                f"DISTINCT/compound workload cases[{index}].diagnostic_probe_groups "
                "do not match the pinned per-tag calls"
            )
        expected = case.get("expected")
        if not isinstance(expected, dict):
            raise HarnessError(
                f"DISTINCT/compound workload cases[{index}].expected must be an object"
            )
        common._require_exact_keys(
            expected,
            base._EXPECTED_KEYS,
            f"DISTINCT/compound workload cases[{index}].expected",
        )
        for group in sorted(base._EXPECTED_KEYS):
            common._validate_work(
                expected[group],
                f"DISTINCT/compound workload cases[{index}].expected.{group}",
            )
        result_rows, result_bytes, single_digest = CASE_SINGLE_RESULTS[case_id]
        measured_digest = CASE_MEASURED_DIGESTS.get(case_id, single_digest)
        expected_groups = {
            "warmup": {
                "operations": 1,
                "items": expected_items,
                "rows": result_rows,
                "bytes": result_bytes,
                "result_hits": 1,
                "result_misses": 0,
                "digest": single_digest,
            },
            "measured": {
                "operations": expected_measured_iterations,
                "items": expected_measured_iterations * expected_items,
                "rows": expected_measured_iterations * result_rows,
                "bytes": expected_measured_iterations * result_bytes,
                "result_hits": expected_measured_iterations,
                "result_misses": 0,
                "digest": measured_digest,
            },
            "diagnostic": {
                "operations": 1,
                "items": expected_items,
                "rows": result_rows,
                "bytes": result_bytes,
                "result_hits": 1,
                "result_misses": 0,
                "digest": single_digest,
            },
            "smoke": {
                "operations": 1,
                "items": expected_items,
                "rows": result_rows,
                "bytes": result_bytes,
                "result_hits": 1,
                "result_misses": 0,
                "digest": single_digest,
            },
            "verification": VERIFICATION_WORK,
        }
        if expected != expected_groups:
            raise HarnessError(
                f"DISTINCT/compound workload cases[{index}].expected "
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

    normalized = copy.deepcopy(value)
    normalized["guard"] = {
        "maximum_cpu_ratio": {"numerator": 10, "denominator": 1},
        "maximum_wall_ratio": {"numerator": 10, "denominator": 1},
    }
    for case in normalized["cases"]:
        if not isinstance(case, dict) or case.get("id") not in CASE_ITEM_COUNTS:
            continue
        case.pop("diagnostic_probe_groups", None)
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
        if isinstance(expected.get("smoke"), dict):
            expected["measured"] = copy.deepcopy(expected["smoke"])
    with _configured_base():
        base.validate_workload_manifest(
            normalized,
            repository_root=repository_root,
            manifest_path=manifest_path,
        )
    return value


def _validate_report_versions(
    value: dict[str, Any],
    fields: tuple[str, ...],
    label: str,
) -> None:
    for field in fields:
        version = common._require_integer(value.get(field), f"{label}.{field}")
        if version != 1:
            raise HarnessError(f"{label}.{field} must be 1")


def validate_raw_timing_report(
    value: Any,
    *,
    workload_manifest: dict[str, Any],
    expected_engine: str,
    expected_case: str,
    expected_run_kind: str = "baseline",
) -> dict[str, Any]:
    common._require_type(value, dict, "DISTINCT/compound timing report")
    common._require_exact_keys(
        value,
        common._RAW_TIMING_KEYS,
        "DISTINCT/compound timing report",
    )
    _validate_report_versions(
        value,
        (
            "schema_version",
            "completion_schema_version",
            "workload_semantics_version",
        ),
        "DISTINCT/compound timing report",
    )
    case = common._case_by_id(workload_manifest, expected_case)
    _require_exact_typed(
        value["effective_configuration"],
        base._expected_case_configuration(workload_manifest, case),
        "DISTINCT/compound timing report.effective_configuration",
    )
    return base.validate_raw_timing_report(
        value,
        workload_manifest=workload_manifest,
        expected_engine=expected_engine,
        expected_case=expected_case,
        expected_run_kind=expected_run_kind,
    )


def validate_raw_diagnostic_report(
    value: Any,
    *,
    workload_manifest: dict[str, Any],
    expected_engine: str,
    expected_case: str,
) -> dict[str, Any]:
    common._require_type(value, dict, "DISTINCT/compound diagnostic report")
    expected_keys = set(common._RAW_DIAGNOSTIC_KEYS) | {
        "probe_counts",
        "source_rows",
    }
    if set(value) != expected_keys:
        raise HarnessError("DISTINCT/compound diagnostic report keys are invalid")
    _validate_report_versions(
        value,
        (
            "schema_version",
            "completion_schema_version",
            "diagnostic_schema_version",
            "workload_semantics_version",
        ),
        "DISTINCT/compound diagnostic report",
    )
    common._require_type(
        value["counters"],
        dict,
        "DISTINCT/compound diagnostic report.counters",
    )
    common._require_exact_keys(
        value["counters"],
        common._COUNTER_GROUP_KEYS,
        "DISTINCT/compound diagnostic report.counters",
    )
    case = common._case_by_id(workload_manifest, expected_case)
    _require_exact_typed(
        value["effective_configuration"],
        base._expected_case_configuration(workload_manifest, case),
        "DISTINCT/compound diagnostic report.effective_configuration",
    )
    probe_counts = value.get("probe_counts")
    common._require_type(
        probe_counts,
        list,
        "DISTINCT/compound diagnostic probe_counts",
    )
    for index, count in enumerate(probe_counts):
        common._require_integer(
            count,
            f"DISTINCT/compound diagnostic probe_counts[{index}]",
        )
    if probe_counts != expected_probe_counts(expected_case):
        raise HarnessError(
            "DISTINCT/compound diagnostic probe_counts do not match the pinned tags"
        )
    source_rows = common._require_integer(
        value.get("source_rows"),
        "DISTINCT/compound diagnostic source_rows",
    )
    if source_rows != sum(expected_probe_counts(expected_case)):
        raise HarnessError(
            "DISTINCT/compound diagnostic source_rows do not match the pinned probe count"
        )
    normalized = copy.deepcopy(value)
    normalized.pop("probe_counts")
    normalized.pop("source_rows")
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
        spilled = counters["pages_written"] > 0
        if spilled != (expected_case in MODERN_SPILL_CASES):
            raise HarnessError(
                "Modern DISTINCT/compound spill classification does not match the case"
            )
    else:
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
    common.validate_raw_timing_report = validate_raw_timing_report
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
