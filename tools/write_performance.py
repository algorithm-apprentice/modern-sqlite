#!/usr/bin/env python3

from __future__ import annotations

import argparse
import ctypes
import dataclasses
import hashlib
import json
import os
import pathlib
import re
import sys
from typing import Any


class HarnessError(RuntimeError):
    pass


class ChildRunFailure(HarnessError):
    def __init__(
        self,
        message: str,
        *,
        command: tuple[str, ...],
        returncode: int,
        stdout: bytes,
        stderr: bytes,
        elapsed_ns: int,
        timed_out: bool,
    ) -> None:
        super().__init__(message)
        self.command = command
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.elapsed_ns = elapsed_ns
        self.timed_out = timed_out


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise HarnessError(f"{self.prog}: error: {message}")


SCHEMA_VERSION = 1
WORKLOAD_SEMANTICS_VERSION = 1
SQLITE_PROFILE = "sqlite-oracle-profile-v1"
SQLITE_VERSION = "3.54.0"
SQLITE_SOURCE_ID = (
    "2026-10-02 20:18:07 "
    "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
)
MINIMUM_WALL_NS = 20_000_000
TIMING_REPETITIONS = 3

_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_DIGEST = re.compile(r"[0-9a-f]{16}\Z")

_TOP_KEYS = {
    "schema_version",
    "workload_semantics_version",
    "sqlite_profile",
    "sqlite_semantic_compile_options",
    "configuration",
    "diagnostic_work",
    "profiles",
    "rounds",
    "timing_repetitions",
    "minimum_wall_ns",
    "fixtures",
    "cases",
    "guard",
    "key_order",
}
_CONFIGURATION_KEYS = {
    "page_size",
    "cache_pages",
    "mmap_bytes",
    "temp_store",
    "journal_mode",
    "matched_synchronous",
    "locking_mode",
    "thread_mode",
}
_PROFILE_KEYS = {"id", "guard", "sqlite_configuration"}
_ROUND_KEYS = {"index", "case_order", "engine_order"}
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
    "value_size",
    "content_digest",
    "freelist_count",
    "schema_cookie",
}
_CASE_KEYS = {
    "id",
    "kind",
    "fixture",
    "primary_unit",
    "sql",
    "expected",
    "transactions",
    "dml_operations",
    "row_mutations",
}
_GUARD_KEYS = {
    "profile",
    "maximum_wall_ratio",
    "maximum_cpu_ratio",
}
_RATIO_KEYS = {"numerator", "denominator"}
_KEY_ORDER_KEYS = {"algorithm", "seed"}
_EXPECTED_GROUP_KEYS = {"smoke", "baseline"}
_WORK_KEYS = {
    "transactions",
    "dml_operations",
    "row_mutations",
    "changed_rows",
    "final_rows",
    "last_insert_rowid",
    "schema_objects",
    "digest",
}
_DATABASE_FINGERPRINT_KEYS = {
    "sha256",
    "size_bytes",
    "page_count",
    "freelist_count",
    "schema_cookie",
}
_REPORT_WORK_KEYS = _WORK_KEYS | {"final_database"}
_RAW_TIMING_KEYS = {
    "build",
    "case",
    "completion",
    "effective_configuration",
    "engine",
    "initial_database",
    "mode",
    "profile",
    "repetitions",
    "run_kind",
    "schema_version",
    "source",
    "sqlite",
    "timer",
    "warmup",
    "workload_semantics_version",
}
_EFFECTIVE_CONFIGURATION_KEYS = {
    "page_size",
    "cache_size",
    "mmap_bytes",
    "temp_store",
    "journal_mode",
    "synchronous",
    "locking_mode",
    "thread_mode",
}
_REPETITION_KEYS = _REPORT_WORK_KEYS | {"index", "wall_ns", "cpu_ns"}
_COMPLETION_KEYS = {
    "fresh_databases",
    "measured_repetitions",
    "post_verifications",
    "pre_verifications",
    "status",
    "warmups",
}
_TIMER_KEYS = {"wall", "cpu"}
_RAW_DIAGNOSTIC_KEYS = {
    "build",
    "case",
    "completion",
    "counters",
    "diagnostic_schema_version",
    "effective_configuration",
    "engine",
    "initial_database",
    "mode",
    "profile",
    "schema_version",
    "source",
    "sqlite",
    "work",
    "workload_semantics_version",
}
_DIAGNOSTIC_COMPLETION_KEYS = {
    "diagnostic_runs",
    "fresh_databases",
    "post_verifications",
    "pre_verifications",
    "status",
    "warmups",
}
_COUNTER_GROUP_KEYS = {"modern", "sqlite", "vfs"}
MODERN_COUNTER_NAMES = (
    "allocations",
    "bytes_copied",
    "vfs_calls",
    "pages_read",
    "pages_written",
    "cache_hits",
    "cache_misses",
    "btree_comparisons",
    "vm_instructions",
    "planner_work",
)
SQLITE_COUNTER_NAMES = (
    "cache_bytes_current",
    "cache_hits",
    "cache_misses",
    "cache_writes",
    "changes",
    "fullscan_steps",
    "malloc_count_current",
    "malloc_count_highwater",
    "malloc_size_highwater",
    "reprepares",
    "statement_runs",
    "total_changes",
    "vm_steps",
)
VFS_FILE_COUNTER_NAMES = (
    "open_calls",
    "close_calls",
    "read_calls",
    "read_bytes",
    "write_calls",
    "write_bytes",
    "sync_calls",
    "truncate_calls",
    "delete_calls",
    "directory_sync_requests",
    "lock_calls",
    "unlock_calls",
    "access_calls",
    "full_path_calls",
)
_VFS_GROUP_KEYS = {
    "global",
    "main_database",
    "main_journal",
    "subjournal",
    "write_ahead_log",
}
_VFS_GLOBAL_KEYS = {"random_byte_calls"}
_BUILD_KEYS = {
    "architecture",
    "build_type",
    "compiler",
    "cplusplus",
    "coverage",
    "instrumentation",
    "sanitizers",
    "standard_library",
}
_TOOLCHAIN_KEYS = {"id", "version"}
_SOURCE_KEYS = {"revision", "tree"}
_SQLITE_IDENTITY_KEYS = {"compile_options", "source_id", "version"}
_IDENTITY_KEYS = {"build", "mode", "schema_version", "source", "sqlite"}
_GIT_OBJECT = re.compile(r"[0-9a-f]{40,64}\Z")
_ARTIFACT_KEYS = {"path", "sha256", "size_bytes"}
_INPUT_KEYS = {"role", "path", "sha256", "size_bytes"}
_BINARY_REFERENCE_KEYS = {"path", "sha256", "size_bytes", "identity"}
_PROCESS_KEYS = {"returncode", "elapsed_ns"}
_TIMING_RUN_KEYS = {
    "ordinal",
    "profile",
    "round",
    "case",
    "engine",
    "command",
    "process",
    "stdout",
    "stderr",
}
_DIAGNOSTIC_RUN_KEYS = _TIMING_RUN_KEYS - {"round"}
_RUN_MANIFEST_KEYS = {
    "schema_version",
    "status",
    "source",
    "host",
    "inputs",
    "binaries",
    "timing_runs",
    "diagnostic_runs",
    "aggregate",
}
_SOURCE_STATE_KEYS = {
    "revision",
    "tree",
    "clean",
    "status_sha256",
    "worktree_content_sha256",
}
_HOST_KEYS = {
    "os",
    "kernel",
    "architecture",
    "cpu",
    "logical_cpu_count",
    "wall_timer",
    "cpu_timer",
}

_EXPECTED_CONFIGURATION = {
    "page_size": 4096,
    "cache_pages": 512,
    "mmap_bytes": 0,
    "temp_store": "memory",
    "journal_mode": "delete",
    "matched_synchronous": "full",
    "locking_mode": "normal",
    "thread_mode": "single",
}
_EXPECTED_PROFILES = (
    {
        "id": "engine-default",
        "guard": False,
        "sqlite_configuration": "default",
    },
    {
        "id": "matched-durable",
        "guard": True,
        "sqlite_configuration": "explicit",
    },
)
_EXPECTED_ROUNDS = (
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
)
_EXPECTED_KEY_ORDER = {
    "algorithm": "splitmix64-rejection-fisher-yates-v1",
    "seed": "d1b54a32d192ed03",
}
_EXPECTED_DIGESTS = {
    "create-table-implicit": {
        "smoke": "424687ee21154511",
        "baseline": "67829c991b8509a9",
    },
    "insert-point-implicit": {
        "smoke": "e0552c11f499c717",
        "baseline": "2138add81b82284d",
    },
    "insert-batch-explicit": {
        "smoke": "d0bea8480021ad75",
        "baseline": "32312236c967f3ff",
    },
    "update-point-implicit": {
        "smoke": "0a45750b4534b536",
        "baseline": "518e297c1461308c",
    },
    "update-scan-implicit": {
        "smoke": "dc2ea3dddf5856ff",
        "baseline": "2821e47c2a7bca80",
    },
    "delete-point-implicit": {
        "smoke": "7839b461492409a8",
        "baseline": "6de42b7c34551b32",
    },
    "delete-scan-implicit": {
        "smoke": "ff71062fc55ae92f",
        "baseline": "cbf29ce484222325",
    },
    "mixed-batch-commit": {
        "smoke": "132249e7b75e8215",
        "baseline": "43efe28707af1ec6",
    },
    "mixed-batch-rollback": {
        "smoke": "32312236c967f3ff",
        "baseline": "32312236c967f3ff",
    },
}
_MATCHED_CONFIGURATION = {
    "page_size": 4096,
    "cache_size": 512,
    "mmap_bytes": 0,
    "temp_store": "memory",
    "journal_mode": "delete",
    "synchronous": "full",
    "locking_mode": "normal",
    "thread_mode": "single",
}
_SQLITE_DEFAULT_CONFIGURATION = {
    **_MATCHED_CONFIGURATION,
    "cache_size": -2000,
    "temp_store": "default",
}
_SQLITE_SEMANTIC_COMPILE_OPTIONS = [
    "ATOMIC_INTRINSICS=1",
    "DEFAULT_AUTOVACUUM",
    "DEFAULT_CACHE_SIZE=-2000",
    "DEFAULT_FILE_FORMAT=4",
    "DEFAULT_JOURNAL_SIZE_LIMIT=-1",
    "DEFAULT_MMAP_SIZE=0",
    "DEFAULT_PAGE_SIZE=4096",
    "DEFAULT_PCACHE_INITSZ=20",
    "DEFAULT_RECURSIVE_TRIGGERS",
    "DEFAULT_SECTOR_SIZE=4096",
    "DEFAULT_SYNCHRONOUS=2",
    "DEFAULT_WAL_AUTOCHECKPOINT=1000",
    "DEFAULT_WAL_SYNCHRONOUS=2",
    "DEFAULT_WORKER_THREADS=0",
    "DIRECT_OVERFLOW_READ",
    "DQS=3",
    "MALLOC_SOFT_LIMIT=1024",
    "MAX_ATTACHED=10",
    "MAX_COLUMN=2000",
    "MAX_COMPOUND_SELECT=500",
    "MAX_DEFAULT_PAGE_SIZE=8192",
    "MAX_EXPR_DEPTH=1000",
    "MAX_FUNCTION_ARG=1000",
    "MAX_LENGTH=1000000000",
    "MAX_LIKE_PATTERN_LENGTH=50000",
    "MAX_MMAP_SIZE=0x7fff0000",
    "MAX_PAGE_COUNT=0xfffffffe",
    "MAX_PAGE_SIZE=65536",
    "MAX_SCHEMA=10000000",
    "MAX_SQL_LENGTH=1000000000",
    "MAX_TRIGGER_DEPTH=1000",
    "MAX_VARIABLE_NUMBER=32766",
    "MAX_VDBE_OP=250000000",
    "MAX_WORKER_THREADS=0",
    "MUTEX_OMIT",
    "SYSTEM_MALLOC",
    "TEMP_STORE=1",
    "THREADSAFE=0",
]
_EXPECTED_FIXTURES = {
    "schema": {
        "path": "tests/fixtures/write_performance/schema.db",
        "sql_path": "tests/fixtures/write_performance/schema.sql",
        "row_count": 0,
    },
    "populated": {
        "path": "tests/fixtures/write_performance/populated.db",
        "sql_path": "tests/fixtures/write_performance/populated.sql",
        "row_count": 65_536,
    },
}
_EXPECTED_CASES = {
    "create-table-implicit": {
        "kind": "create",
        "fixture": "zero",
        "primary_unit": "statement",
        "sql": [
            "CREATE TABLE tNNN("
            "id INTEGER PRIMARY KEY,v BLOB NOT NULL DEFAULT x'')"
        ],
        "transactions": 256,
        "dml_operations": 256,
        "row_mutations": 0,
    },
    "insert-point-implicit": {
        "kind": "insert_point",
        "fixture": "schema",
        "primary_unit": "row",
        "sql": ["INSERT INTO kv(k,v,version) VALUES(?1,?2,0)"],
        "transactions": 512,
        "dml_operations": 512,
        "row_mutations": 512,
    },
    "insert-batch-explicit": {
        "kind": "insert_batch",
        "fixture": "schema",
        "primary_unit": "row",
        "sql": [
            "BEGIN",
            "INSERT INTO kv(k,v,version) VALUES(?1,?2,0)",
            "COMMIT",
        ],
        "transactions": 1,
        "dml_operations": 65_536,
        "row_mutations": 65_536,
    },
    "update-point-implicit": {
        "kind": "update_point",
        "fixture": "populated",
        "primary_unit": "row",
        "sql": ["UPDATE kv SET v=?1,version=version+1 WHERE k=?2"],
        "transactions": 512,
        "dml_operations": 512,
        "row_mutations": 512,
    },
    "update-scan-implicit": {
        "kind": "update_scan",
        "fixture": "populated",
        "primary_unit": "row",
        "sql": ["UPDATE kv SET v=?1,version=version+1 WHERE k>=1"],
        "transactions": 1,
        "dml_operations": 1,
        "row_mutations": 65_536,
    },
    "delete-point-implicit": {
        "kind": "delete_point",
        "fixture": "populated",
        "primary_unit": "row",
        "sql": ["DELETE FROM kv WHERE k=?1"],
        "transactions": 512,
        "dml_operations": 512,
        "row_mutations": 512,
    },
    "delete-scan-implicit": {
        "kind": "delete_scan",
        "fixture": "populated",
        "primary_unit": "row",
        "sql": ["DELETE FROM kv WHERE k>=1"],
        "transactions": 1,
        "dml_operations": 1,
        "row_mutations": 65_536,
    },
    "mixed-batch-commit": {
        "kind": "mixed_commit",
        "fixture": "populated",
        "primary_unit": "row_mutation",
        "sql": [
            "BEGIN",
            "UPDATE kv SET v=?1,version=version+1 WHERE k=?2",
            "DELETE FROM kv WHERE k=?1",
            "INSERT INTO kv(k,v,version) VALUES(?1,?2,0)",
            "COMMIT",
        ],
        "transactions": 1,
        "dml_operations": 12_288,
        "row_mutations": 12_288,
    },
    "mixed-batch-rollback": {
        "kind": "mixed_rollback",
        "fixture": "populated",
        "primary_unit": "row_mutation",
        "sql": [
            "BEGIN",
            "UPDATE kv SET v=?1,version=version+1 WHERE k=?2",
            "DELETE FROM kv WHERE k=?1",
            "INSERT INTO kv(k,v,version) VALUES(?1,?2,0)",
            "ROLLBACK",
        ],
        "transactions": 1,
        "dml_operations": 12_288,
        "row_mutations": 12_288,
    },
}

_FNV_OFFSET = 0xCBF29CE484222325
_FNV_PRIME = 0x100000001B3
_SQLITE_OPEN_READONLY = 0x00000001
_SQLITE_OK = 0
_SQLITE_ROW = 100
_SQLITE_DONE = 101


def _duplicate_rejecting_object(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise HarnessError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def _reject_nonfinite(token: str) -> None:
    raise HarnessError(f"non-finite JSON number is not allowed: {token}")


def _require_type(value: Any, expected: type, label: str) -> None:
    if expected is int:
        if isinstance(value, bool) or not isinstance(value, int):
            raise HarnessError(f"{label} must be an integer")
        return
    if not isinstance(value, expected):
        raise HarnessError(f"{label} must be {expected.__name__}")


def _require_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    if set(value) != expected:
        raise HarnessError(f"{label} has invalid keys")


def _require_positive_integer(value: Any, label: str) -> int:
    _require_type(value, int, label)
    if value <= 0:
        raise HarnessError(f"{label} must be positive")
    return value


def _load_json(path: pathlib.Path) -> dict[str, Any]:
    if not path.is_file():
        raise HarnessError(f"workload manifest does not exist: {path}")
    if path.stat().st_size > 4_194_304:
        raise HarnessError("workload manifest exceeds the byte limit")
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_duplicate_rejecting_object,
            parse_constant=_reject_nonfinite,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise HarnessError(f"could not parse workload manifest: {error}") from error
    _require_type(value, dict, "workload manifest")
    return value


def load_json_bytes_strict(data: bytes, label: str) -> Any:
    if len(data) > 4 * 1024 * 1024:
        raise HarnessError(f"{label} exceeds the byte limit")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise HarnessError(f"{label} is not UTF-8") from error
    try:
        return json.loads(
            text,
            object_pairs_hook=_duplicate_rejecting_object,
            parse_constant=_reject_nonfinite,
        )
    except HarnessError:
        raise
    except json.JSONDecodeError as error:
        raise HarnessError(f"invalid JSON from {label}: {error}") from error


def _read_performance_module() -> Any:
    if __package__:
        from tools import read_performance
    else:
        import read_performance

    return read_performance


def _validate_configuration(value: Any) -> None:
    _require_type(value, dict, "configuration")
    _require_keys(value, _CONFIGURATION_KEYS, "configuration")
    for key in ("page_size", "cache_pages", "mmap_bytes"):
        _require_type(value[key], int, f"configuration.{key}")
    for key in (
        "temp_store",
        "journal_mode",
        "matched_synchronous",
        "locking_mode",
        "thread_mode",
    ):
        _require_type(value[key], str, f"configuration.{key}")
    if value != _EXPECTED_CONFIGURATION:
        raise HarnessError("configuration does not match write semantics version 1")


def _validate_profiles(value: Any) -> None:
    _require_type(value, list, "profiles")
    for index, profile in enumerate(value):
        _require_type(profile, dict, f"profiles[{index}]")
        _require_keys(profile, _PROFILE_KEYS, f"profiles[{index}]")
        _require_type(profile["id"], str, f"profiles[{index}].id")
        _require_type(profile["guard"], bool, f"profiles[{index}].guard")
        _require_type(
            profile["sqlite_configuration"],
            str,
            f"profiles[{index}].sqlite_configuration",
        )
    if value != list(_EXPECTED_PROFILES):
        raise HarnessError(
            "profiles must be engine-default then matched-durable"
        )


def _validate_rounds(value: Any) -> None:
    _require_type(value, list, "rounds")
    if len(value) != len(_EXPECTED_ROUNDS):
        raise HarnessError("round count is invalid")
    for index, (actual, expected) in enumerate(
        zip(value, _EXPECTED_ROUNDS, strict=True)
    ):
        _require_type(actual, dict, f"rounds[{index}]")
        _require_keys(actual, _ROUND_KEYS, f"rounds[{index}]")
        _require_type(actual["index"], int, f"rounds[{index}].index")
        _require_type(
            actual["case_order"], str, f"rounds[{index}].case_order"
        )
        _require_type(
            actual["engine_order"], list, f"rounds[{index}].engine_order"
        )
        for engine_index, engine in enumerate(actual["engine_order"]):
            _require_type(
                engine,
                str,
                f"rounds[{index}].engine_order[{engine_index}]",
            )
        if actual["index"] != index:
            raise HarnessError(f"round {index} has the wrong index")
        if actual["case_order"] != expected["case_order"]:
            raise HarnessError(f"round {index} has the wrong case order")
        if actual["engine_order"] != expected["engine_order"]:
            raise HarnessError(f"round {index} has the wrong engine order")


def _validate_fixture(value: Any, index: int) -> str:
    label = f"fixtures[{index}]"
    _require_type(value, dict, label)
    _require_keys(value, _FIXTURE_KEYS, label)
    fixture_id = value["id"]
    _require_type(fixture_id, str, f"{label}.id")
    if fixture_id not in _EXPECTED_FIXTURES:
        raise HarnessError(f"{label}.id is invalid")
    expected = _EXPECTED_FIXTURES[fixture_id]
    for key in ("path", "sql_path"):
        _require_type(value[key], str, f"{label}.{key}")
        if value[key] != expected[key]:
            raise HarnessError(f"{label}.{key} is not canonical")
        path = pathlib.PurePosixPath(value[key])
        if path.is_absolute() or ".." in path.parts:
            raise HarnessError(f"{label}.{key} is not repository-relative")
    for key in ("sha256", "sql_sha256"):
        _require_type(value[key], str, f"{label}.{key}")
        if _SHA256.fullmatch(value[key]) is None:
            raise HarnessError(f"{label}.{key} is not a SHA-256")
    _require_type(value["content_digest"], str, f"{label}.content_digest")
    if _DIGEST.fullmatch(value["content_digest"]) is None:
        raise HarnessError(f"{label}.content_digest is invalid")
    _require_positive_integer(value["size_bytes"], f"{label}.size_bytes")
    _require_type(value["page_size"], int, f"{label}.page_size")
    if value["page_size"] != 4096:
        raise HarnessError(f"{label}.page_size must be 4096")
    _require_positive_integer(value["page_count"], f"{label}.page_count")
    _require_type(value["row_count"], int, f"{label}.row_count")
    if value["row_count"] != expected["row_count"]:
        raise HarnessError(f"{label}.row_count is invalid")
    _require_type(value["value_size"], int, f"{label}.value_size")
    if value["value_size"] != 256:
        raise HarnessError(f"{label}.value_size must be 256")
    _require_type(value["freelist_count"], int, f"{label}.freelist_count")
    if value["freelist_count"] != 0:
        raise HarnessError(f"{label}.freelist_count must be zero")
    _require_type(value["schema_cookie"], int, f"{label}.schema_cookie")
    if value["schema_cookie"] != 1:
        raise HarnessError(f"{label}.schema_cookie must be one")
    return fixture_id


def _validate_fixtures(value: Any) -> None:
    _require_type(value, list, "fixtures")
    if len(value) != 2:
        raise HarnessError("fixture count is invalid")
    ids = [_validate_fixture(fixture, index) for index, fixture in enumerate(value)]
    if ids != ["schema", "populated"]:
        raise HarnessError("fixtures must be schema then populated")
    if value[0]["page_count"] >= 512:
        raise HarnessError("schema fixture must fit in the 512-page cache")
    if value[1]["page_count"] <= 512:
        raise HarnessError("populated fixture must exceed the 512-page cache")


def _canonical_work(case_id: str, run_kind: str) -> dict[str, Any]:
    case = _EXPECTED_CASES[case_id]
    kind = case["kind"]
    if run_kind == "baseline":
        transactions = case["transactions"]
        dml_operations = case["dml_operations"]
        row_mutations = case["row_mutations"]
    elif run_kind == "smoke":
        transactions = 1
        dml_operations = {
            "create": 1,
            "insert_point": 1,
            "insert_batch": 8,
            "update_point": 1,
            "update_scan": 1,
            "delete_point": 1,
            "delete_scan": 1,
            "mixed_commit": 8,
            "mixed_rollback": 8,
        }[kind]
        row_mutations = {
            "create": 0,
            "insert_point": 1,
            "insert_batch": 8,
            "update_point": 1,
            "update_scan": 8,
            "delete_point": 1,
            "delete_scan": 8,
            "mixed_commit": 8,
            "mixed_rollback": 8,
        }[kind]
    else:
        raise HarnessError("run kind must be smoke or baseline")

    final_rows = 65_536
    last_insert_rowid = 0
    schema_objects = 1
    if kind == "create":
        final_rows = 0
        schema_objects = dml_operations
    elif kind in {"insert_point", "insert_batch"}:
        final_rows = row_mutations
        last_insert_rowid = row_mutations
    elif kind in {"delete_point", "delete_scan"}:
        final_rows -= row_mutations
    elif kind in {"mixed_commit", "mixed_rollback"}:
        updates = row_mutations // 3
        deletes = row_mutations // 3
        inserts = row_mutations - updates - deletes
        last_insert_rowid = 65_536 + inserts
        if kind == "mixed_commit":
            final_rows = final_rows - deletes + inserts

    return {
        "transactions": transactions,
        "dml_operations": dml_operations,
        "row_mutations": row_mutations,
        "changed_rows": 0 if kind == "create" else row_mutations,
        "final_rows": final_rows,
        "last_insert_rowid": last_insert_rowid,
        "schema_objects": schema_objects,
        "digest": _EXPECTED_DIGESTS[case_id][run_kind],
    }


def _validate_work_object(value: Any, label: str) -> dict[str, Any]:
    _require_type(value, dict, label)
    _require_keys(value, _WORK_KEYS, label)
    for key in _WORK_KEYS - {"digest"}:
        _require_type(value[key], int, f"{label}.{key}")
        if value[key] < 0:
            raise HarnessError(f"{label}.{key} must be nonnegative")
    _require_type(value["digest"], str, f"{label}.digest")
    if _DIGEST.fullmatch(value["digest"]) is None:
        raise HarnessError(f"{label}.digest is invalid")
    return value


def _validate_database_fingerprint(
    value: Any,
    label: str,
) -> dict[str, Any]:
    _require_type(value, dict, label)
    _require_keys(value, _DATABASE_FINGERPRINT_KEYS, label)
    _require_type(value["sha256"], str, f"{label}.sha256")
    if _SHA256.fullmatch(value["sha256"]) is None:
        raise HarnessError(f"{label}.sha256 is invalid")
    for key in (
        "size_bytes",
        "page_count",
        "freelist_count",
        "schema_cookie",
    ):
        _require_type(value[key], int, f"{label}.{key}")
        if value[key] < 0:
            raise HarnessError(f"{label}.{key} must be nonnegative")
    if value["size_bytes"] == 0:
        if any(
            value[key] != 0
            for key in ("page_count", "freelist_count", "schema_cookie")
        ):
            raise HarnessError(f"{label} zero-byte metadata is inconsistent")
    else:
        if (
            value["size_bytes"] % 4096 != 0
            or value["page_count"] != value["size_bytes"] // 4096
            or value["freelist_count"] > value["page_count"]
        ):
            raise HarnessError(f"{label} page metadata is inconsistent")
    return value


def _fixture_by_id(
    workload_manifest: dict[str, Any],
    fixture_id: str,
) -> dict[str, Any]:
    for fixture in workload_manifest["fixtures"]:
        if fixture["id"] == fixture_id:
            return fixture
    raise HarnessError(f"unknown write performance fixture: {fixture_id}")


def _expected_initial_database(
    workload_manifest: dict[str, Any],
    case: dict[str, Any],
) -> dict[str, Any]:
    if case["fixture"] == "zero":
        return {
            "sha256": hashlib.sha256(b"").hexdigest(),
            "size_bytes": 0,
            "page_count": 0,
            "freelist_count": 0,
            "schema_cookie": 0,
        }
    fixture = _fixture_by_id(workload_manifest, case["fixture"])
    return {
        "sha256": fixture["sha256"],
        "size_bytes": fixture["size_bytes"],
        "page_count": fixture["page_count"],
        "freelist_count": fixture["freelist_count"],
        "schema_cookie": fixture["schema_cookie"],
    }


def _validate_report_work(
    value: Any,
    *,
    expected: dict[str, Any],
    label: str,
) -> dict[str, Any]:
    _require_type(value, dict, label)
    _require_keys(value, _REPORT_WORK_KEYS, label)
    semantic = {key: value[key] for key in _WORK_KEYS}
    _validate_work_object(semantic, f"{label} semantic work")
    if semantic != expected:
        raise HarnessError(f"{label} does not match the workload manifest")
    _validate_database_fingerprint(
        value["final_database"],
        f"{label}.final_database",
    )
    return value


def _validate_final_database_state(
    *,
    case: dict[str, Any],
    work: dict[str, Any],
    initial: dict[str, Any],
    label: str,
) -> None:
    final = work["final_database"]
    expected_cookie = (
        work["dml_operations"]
        if case["kind"] == "create"
        else initial["schema_cookie"]
    )
    if final["schema_cookie"] != expected_cookie:
        raise HarnessError(f"{label} schema cookie is invalid")
    if case["kind"] == "mixed_rollback" and final != initial:
        raise HarnessError(f"{label} rollback database is not byte-identical")


def _validate_expected_work(value: Any, case_id: str) -> None:
    _require_type(value, dict, f"case {case_id}.expected")
    _require_keys(value, _EXPECTED_GROUP_KEYS, f"case {case_id}.expected")
    for run_kind in ("smoke", "baseline"):
        label = f"case {case_id}.expected.{run_kind}"
        work = _validate_work_object(value[run_kind], label)
        if work != _canonical_work(case_id, run_kind):
            raise HarnessError(f"{label} is not canonical")


def _validate_case(value: Any, index: int) -> str:
    label = f"cases[{index}]"
    _require_type(value, dict, label)
    _require_keys(value, _CASE_KEYS, label)
    case_id = value["id"]
    _require_type(case_id, str, f"{label}.id")
    if case_id not in _EXPECTED_CASES:
        raise HarnessError(f"{label}.id is invalid")
    expected = _EXPECTED_CASES[case_id]
    for key in ("kind", "fixture", "primary_unit"):
        _require_type(value[key], str, f"case {case_id}.{key}")
        if value[key] != expected[key]:
            raise HarnessError(f"case {case_id}.{key} is not canonical")
    _require_type(value["sql"], list, f"case {case_id}.sql")
    if value["sql"] != expected["sql"]:
        raise HarnessError(f"case {case_id}.sql is not canonical")
    for sql_index, sql in enumerate(value["sql"]):
        _require_type(sql, str, f"case {case_id}.sql[{sql_index}]")
        if not sql or "\0" in sql:
            raise HarnessError(f"case {case_id}.sql[{sql_index}] is invalid")
    for key in ("transactions", "dml_operations", "row_mutations"):
        _require_type(value[key], int, f"case {case_id}.{key}")
        if value[key] != expected[key]:
            raise HarnessError(f"case {case_id}.{key} is not canonical")
    _validate_expected_work(value["expected"], case_id)
    return case_id


def _validate_cases(value: Any) -> None:
    _require_type(value, list, "cases")
    ids = [_validate_case(case, index) for index, case in enumerate(value)]
    if ids != list(_EXPECTED_CASES):
        raise HarnessError(
            "case IDs do not match workload semantics version 1"
        )


def _validate_ratio(value: Any, label: str) -> None:
    _require_type(value, dict, label)
    _require_keys(value, _RATIO_KEYS, label)
    numerator = _require_positive_integer(value["numerator"], f"{label}.numerator")
    denominator = _require_positive_integer(
        value["denominator"], f"{label}.denominator"
    )
    if numerator != 10 or denominator != 1:
        raise HarnessError(f"{label} must be 10/1")


def _validate_guard(value: Any) -> None:
    _require_type(value, dict, "guard")
    _require_keys(value, _GUARD_KEYS, "guard")
    _require_type(value["profile"], str, "guard.profile")
    if value["profile"] != "matched-durable":
        raise HarnessError("guard.profile must be matched-durable")
    _validate_ratio(value["maximum_wall_ratio"], "guard.maximum_wall_ratio")
    _validate_ratio(value["maximum_cpu_ratio"], "guard.maximum_cpu_ratio")


def _validate_key_order(value: Any) -> None:
    _require_type(value, dict, "key_order")
    _require_keys(value, _KEY_ORDER_KEYS, "key_order")
    for key in _KEY_ORDER_KEYS:
        _require_type(value[key], str, f"key_order.{key}")
    if value != _EXPECTED_KEY_ORDER:
        raise HarnessError("key_order is not canonical")


def load_and_validate_workloads(path: pathlib.Path) -> dict[str, Any]:
    value = _load_json(path)
    _require_keys(value, _TOP_KEYS, "workload manifest")
    _require_type(value["schema_version"], int, "schema_version")
    if value["schema_version"] != SCHEMA_VERSION:
        raise HarnessError("schema_version must be 1")
    _require_type(
        value["workload_semantics_version"],
        int,
        "workload_semantics_version",
    )
    if value["workload_semantics_version"] != WORKLOAD_SEMANTICS_VERSION:
        raise HarnessError("workload_semantics_version must be 1")
    _require_type(value["sqlite_profile"], str, "sqlite_profile")
    if value["sqlite_profile"] != SQLITE_PROFILE:
        raise HarnessError("sqlite_profile is invalid")
    _require_type(
        value["sqlite_semantic_compile_options"],
        list,
        "sqlite_semantic_compile_options",
    )
    if value["sqlite_semantic_compile_options"] != (
        _SQLITE_SEMANTIC_COMPILE_OPTIONS
    ):
        raise HarnessError("sqlite_semantic_compile_options are invalid")
    _validate_configuration(value["configuration"])
    _require_type(value["diagnostic_work"], str, "diagnostic_work")
    if value["diagnostic_work"] != "baseline":
        raise HarnessError("diagnostic_work must be baseline")
    _validate_profiles(value["profiles"])
    _validate_rounds(value["rounds"])
    _require_type(value["timing_repetitions"], int, "timing_repetitions")
    if value["timing_repetitions"] != TIMING_REPETITIONS:
        raise HarnessError("timing_repetitions must be 3")
    _require_type(value["minimum_wall_ns"], int, "minimum_wall_ns")
    if value["minimum_wall_ns"] != MINIMUM_WALL_NS:
        raise HarnessError("minimum_wall_ns must be 20000000")
    _validate_fixtures(value["fixtures"])
    _validate_cases(value["cases"])
    _validate_guard(value["guard"])
    _validate_key_order(value["key_order"])
    return value


def validate_workloads(path: pathlib.Path) -> tuple[int, int]:
    value = load_and_validate_workloads(path)
    return len(value["cases"]), len(value["profiles"])


def _case_by_id(
    workload_manifest: dict[str, Any],
    case_id: str,
) -> dict[str, Any]:
    for case in workload_manifest["cases"]:
        if case["id"] == case_id:
            return case
    raise HarnessError(f"unknown write performance case: {case_id}")


def _validate_build_identity(
    value: Any,
    *,
    instrumentation: bool,
    label: str,
) -> dict[str, Any]:
    _require_type(value, dict, label)
    _require_keys(value, _BUILD_KEYS, label)
    if value["architecture"] not in {"arm64", "x86_64"}:
        raise HarnessError(f"{label}.architecture is invalid")
    if value["build_type"] != "Release":
        raise HarnessError(f"{label}.build_type must be Release")
    _require_type(value["cplusplus"], int, f"{label}.cplusplus")
    if value["cplusplus"] < 202100:
        raise HarnessError(f"{label}.cplusplus must be C++23")
    for key in ("coverage", "instrumentation", "sanitizers"):
        _require_type(value[key], bool, f"{label}.{key}")
    if (
        value["coverage"]
        or value["sanitizers"]
        or value["instrumentation"] is not instrumentation
    ):
        raise HarnessError(f"{label} instrumentation flags are invalid")
    for key, valid_ids in (
        ("compiler", {"clang", "gcc", "msvc"}),
        ("standard_library", {"libc++", "libstdc++", "msvc-stl"}),
    ):
        toolchain = value[key]
        toolchain_label = f"{label}.{key}"
        _require_type(toolchain, dict, toolchain_label)
        _require_keys(toolchain, _TOOLCHAIN_KEYS, toolchain_label)
        if toolchain["id"] not in valid_ids:
            raise HarnessError(f"{toolchain_label}.id is invalid")
        _require_type(
            toolchain["version"],
            str,
            f"{toolchain_label}.version",
        )
        if not toolchain["version"]:
            raise HarnessError(f"{toolchain_label}.version must be nonempty")
    return value


def _validate_source_identity(value: Any, label: str) -> dict[str, str]:
    _require_type(value, dict, label)
    _require_keys(value, _SOURCE_KEYS, label)
    for key in _SOURCE_KEYS:
        _require_type(value[key], str, f"{label}.{key}")
        if _GIT_OBJECT.fullmatch(value[key]) is None:
            raise HarnessError(f"{label}.{key} is not a Git object ID")
    return value


def _validate_sqlite_identity(value: Any, label: str) -> dict[str, Any]:
    _require_type(value, dict, label)
    _require_keys(value, _SQLITE_IDENTITY_KEYS, label)
    if value["version"] != SQLITE_VERSION:
        raise HarnessError(f"{label}.version is not pinned")
    if value["source_id"] != SQLITE_SOURCE_ID:
        raise HarnessError(f"{label}.source_id is not pinned")
    if value["compile_options"] != _SQLITE_SEMANTIC_COMPILE_OPTIONS:
        raise HarnessError(f"{label}.compile_options are not pinned")
    return value


def validate_binary_identity(
    value: Any,
    *,
    instrumentation: bool,
) -> dict[str, Any]:
    _require_type(value, dict, "binary identity")
    _require_keys(value, _IDENTITY_KEYS, "binary identity")
    _require_type(
        value["schema_version"],
        int,
        "binary identity.schema_version",
    )
    if value["schema_version"] != SCHEMA_VERSION:
        raise HarnessError("binary identity schema_version must be 1")
    if value["mode"] != "identity":
        raise HarnessError("binary identity mode must be identity")
    _validate_build_identity(
        value["build"],
        instrumentation=instrumentation,
        label="binary identity.build",
    )
    _validate_source_identity(value["source"], "binary identity.source")
    _validate_sqlite_identity(value["sqlite"], "binary identity.sqlite")
    return value


def read_binary_identity(
    *,
    binary_path: pathlib.Path,
    repository_root: pathlib.Path,
    instrumentation: bool,
) -> dict[str, Any]:
    if not binary_path.is_file():
        raise HarnessError(f"benchmark binary does not exist: {binary_path}")
    common = _read_performance_module()
    try:
        result = common.run_bounded(
            [str(binary_path), "identity"],
            cwd=repository_root,
            timeout_seconds=30.0,
            stdout_limit=1024 * 1024,
            stderr_limit=1024 * 1024,
        )
    except (common.HarnessError, common.ChildExecutionError) as error:
        raise HarnessError(str(error)) from error
    if result.returncode != 0:
        raise HarnessError(
            f"benchmark identity failed with exit {result.returncode}"
        )
    if result.stderr:
        raise HarnessError("benchmark identity wrote stderr")
    identity = load_json_bytes_strict(result.stdout, "benchmark identity")
    return validate_binary_identity(
        identity,
        instrumentation=instrumentation,
    )


def _validate_effective_configuration(
    value: Any,
    *,
    engine: str,
    profile: str,
) -> dict[str, Any]:
    _require_type(value, dict, "timing report.effective_configuration")
    _require_keys(
        value,
        _EFFECTIVE_CONFIGURATION_KEYS,
        "timing report.effective_configuration",
    )
    for key in ("page_size", "cache_size", "mmap_bytes"):
        _require_type(
            value[key],
            int,
            f"timing report.effective_configuration.{key}",
        )
    for key in _EFFECTIVE_CONFIGURATION_KEYS - {
        "page_size",
        "cache_size",
        "mmap_bytes",
    }:
        _require_type(
            value[key],
            str,
            f"timing report.effective_configuration.{key}",
        )
    expected = (
        _SQLITE_DEFAULT_CONFIGURATION
        if engine == "sqlite" and profile == "engine-default"
        else _MATCHED_CONFIGURATION
    )
    if value != expected:
        raise HarnessError(
            "timing report effective configuration does not match "
            "the engine and profile"
        )
    return value


def validate_raw_timing_report(
    value: Any,
    *,
    workload_manifest: dict[str, Any],
    expected_engine: str,
    expected_profile: str,
    expected_case: str,
    expected_run_kind: str,
) -> dict[str, Any]:
    _require_type(value, dict, "timing report")
    _require_keys(value, _RAW_TIMING_KEYS, "timing report")
    _require_type(
        value["schema_version"],
        int,
        "timing report.schema_version",
    )
    if value["schema_version"] != SCHEMA_VERSION:
        raise HarnessError("timing report schema_version must be 1")
    _require_type(
        value["workload_semantics_version"],
        int,
        "timing report.workload_semantics_version",
    )
    if value["workload_semantics_version"] != WORKLOAD_SEMANTICS_VERSION:
        raise HarnessError(
            "timing report workload_semantics_version must be 1"
        )
    if expected_engine not in {"modern", "sqlite"}:
        raise HarnessError("expected engine must be modern or sqlite")
    if expected_profile not in {"engine-default", "matched-durable"}:
        raise HarnessError("expected profile is invalid")
    if expected_run_kind not in {"smoke", "baseline"}:
        raise HarnessError("expected run kind must be smoke or baseline")
    _require_type(value["mode"], str, "timing report.mode")
    if value["mode"] != "timing":
        raise HarnessError("timing report mode must be timing")
    for key, expected in (
        ("engine", expected_engine),
        ("profile", expected_profile),
        ("case", expected_case),
        ("run_kind", expected_run_kind),
    ):
        _require_type(value[key], str, f"timing report.{key}")
        if value[key] != expected:
            raise HarnessError(
                f"timing report {key} does not match the invocation"
            )
    _validate_build_identity(
        value["build"],
        instrumentation=False,
        label="timing report.build",
    )
    _validate_source_identity(value["source"], "timing report.source")
    _validate_sqlite_identity(value["sqlite"], "timing report.sqlite")

    case = _case_by_id(workload_manifest, expected_case)
    expected_work = case["expected"][expected_run_kind]
    initial_database = _validate_database_fingerprint(
        value["initial_database"],
        "timing report.initial_database",
    )
    if initial_database != _expected_initial_database(
        workload_manifest,
        case,
    ):
        raise HarnessError(
            "timing report initial database does not match the fixture"
        )
    _validate_effective_configuration(
        value["effective_configuration"],
        engine=expected_engine,
        profile=expected_profile,
    )
    timer = value["timer"]
    _require_type(timer, dict, "timing report.timer")
    _require_keys(timer, _TIMER_KEYS, "timing report.timer")
    if timer != {
        "wall": "steady_clock",
        "cpu": "CLOCK_PROCESS_CPUTIME_ID",
    }:
        raise HarnessError("timing report timer identities are invalid")

    warmup = _validate_report_work(
        value["warmup"],
        expected=expected_work,
        label="timing report warmup",
    )
    _validate_final_database_state(
        case=case,
        work=warmup,
        initial=initial_database,
        label="timing report warmup",
    )

    repetition_count = 1 if expected_run_kind == "smoke" else 3
    repetitions = value["repetitions"]
    _require_type(repetitions, list, "timing report.repetitions")
    if len(repetitions) != repetition_count:
        raise HarnessError(
            "timing report repetitions have the wrong count"
        )
    indexes = []
    for position, repetition in enumerate(repetitions):
        label = f"timing report.repetitions[{position}]"
        _require_type(repetition, dict, label)
        _require_keys(repetition, _REPETITION_KEYS, label)
        index = repetition["index"]
        wall_ns = repetition["wall_ns"]
        cpu_ns = repetition["cpu_ns"]
        _require_type(index, int, f"{label}.index")
        _require_type(wall_ns, int, f"{label}.wall_ns")
        _require_type(cpu_ns, int, f"{label}.cpu_ns")
        if index < 0:
            raise HarnessError(f"{label}.index must be nonnegative")
        if wall_ns <= 0:
            raise HarnessError(f"{label}.wall_ns must be positive")
        if cpu_ns < 0:
            raise HarnessError(f"{label}.cpu_ns must be nonnegative")
        if (
            expected_run_kind == "baseline"
            and wall_ns < workload_manifest["minimum_wall_ns"]
        ):
            raise HarnessError(
                f"{label} is below the "
                f"{workload_manifest['minimum_wall_ns']} minimum wall time"
            )
        work = {key: repetition[key] for key in _REPORT_WORK_KEYS}
        _validate_report_work(
            work,
            expected=expected_work,
            label=label,
        )
        _validate_final_database_state(
            case=case,
            work=work,
            initial=initial_database,
            label=label,
        )
        indexes.append(index)
    if indexes != list(range(repetition_count)):
        raise HarnessError(
            "timing report repetition indexes are incomplete or duplicated"
        )

    completion = value["completion"]
    _require_type(completion, dict, "timing report.completion")
    _require_keys(completion, _COMPLETION_KEYS, "timing report.completion")
    for key in _COMPLETION_KEYS - {"status"}:
        _require_type(
            completion[key],
            int,
            f"timing report.completion.{key}",
        )
        if completion[key] < 0:
            raise HarnessError(
                f"timing report.completion.{key} must be nonnegative"
            )
    _require_type(
        completion["status"],
        str,
        "timing report.completion.status",
    )
    expected_completion = {
        "fresh_databases": repetition_count + 1,
        "measured_repetitions": repetition_count,
        "post_verifications": repetition_count + 1,
        "pre_verifications": 1,
        "status": "complete",
        "warmups": 1,
    }
    if completion != expected_completion:
        raise HarnessError(
            "timing report completion does not match the run kind"
        )
    return value


def _validate_counter_values(
    value: Any,
    *,
    names: tuple[str, ...],
    label: str,
) -> dict[str, int]:
    _require_type(value, dict, label)
    if set(value) != set(names):
        family = "Modern" if names == MODERN_COUNTER_NAMES else "SQLite"
        raise HarnessError(f"{family} counter names are invalid")
    for name in names:
        _require_type(value[name], int, f"{label}.{name}")
        if value[name] < 0:
            raise HarnessError(f"{label}.{name} must be nonnegative")
    return value


def _validate_vfs_counters(value: Any) -> dict[str, Any]:
    _require_type(value, dict, "diagnostic report.counters.vfs")
    _require_keys(value, _VFS_GROUP_KEYS, "diagnostic report.counters.vfs")
    global_counters = value["global"]
    _require_type(
        global_counters,
        dict,
        "diagnostic report.counters.vfs.global",
    )
    _require_keys(
        global_counters,
        _VFS_GLOBAL_KEYS,
        "diagnostic report.counters.vfs.global",
    )
    _require_type(
        global_counters["random_byte_calls"],
        int,
        "diagnostic report.counters.vfs.global.random_byte_calls",
    )
    if global_counters["random_byte_calls"] < 0:
        raise HarnessError(
            "diagnostic report.counters.vfs.global.random_byte_calls "
            "must be nonnegative"
        )
    for group in (
        "main_database",
        "main_journal",
        "subjournal",
        "write_ahead_log",
    ):
        label = f"diagnostic report.counters.vfs.{group}"
        counters = value[group]
        _require_type(counters, dict, label)
        if set(counters) != set(VFS_FILE_COUNTER_NAMES):
            raise HarnessError(f"{label} counter names are invalid")
        for name in VFS_FILE_COUNTER_NAMES:
            _require_type(counters[name], int, f"{label}.{name}")
            if counters[name] < 0:
                raise HarnessError(f"{label}.{name} must be nonnegative")
        if counters["open_calls"] != counters["close_calls"]:
            raise HarnessError(f"{label} open and close counts differ")
    if value["main_database"]["open_calls"] <= 0:
        raise HarnessError(
            "Modern diagnostic main-database open count must be positive"
        )
    return value


def validate_raw_diagnostic_report(
    value: Any,
    *,
    workload_manifest: dict[str, Any],
    expected_engine: str,
    expected_profile: str,
    expected_case: str,
) -> dict[str, Any]:
    _require_type(value, dict, "diagnostic report")
    _require_keys(value, _RAW_DIAGNOSTIC_KEYS, "diagnostic report")
    for key in (
        "schema_version",
        "diagnostic_schema_version",
        "workload_semantics_version",
    ):
        _require_type(value[key], int, f"diagnostic report.{key}")
        if value[key] != 1:
            raise HarnessError(f"diagnostic report {key} must be 1")
    if expected_engine not in {"modern", "sqlite"}:
        raise HarnessError("expected engine must be modern or sqlite")
    if expected_profile not in {"engine-default", "matched-durable"}:
        raise HarnessError("expected profile is invalid")
    _require_type(value["mode"], str, "diagnostic report.mode")
    if value["mode"] != "diagnostic":
        raise HarnessError("diagnostic report mode must be diagnostic")
    for key, expected in (
        ("engine", expected_engine),
        ("profile", expected_profile),
        ("case", expected_case),
    ):
        _require_type(value[key], str, f"diagnostic report.{key}")
        if value[key] != expected:
            raise HarnessError(
                f"diagnostic report {key} does not match the invocation"
            )
    _validate_build_identity(
        value["build"],
        instrumentation=True,
        label="diagnostic report.build",
    )
    _validate_source_identity(value["source"], "diagnostic report.source")
    _validate_sqlite_identity(value["sqlite"], "diagnostic report.sqlite")

    case = _case_by_id(workload_manifest, expected_case)
    expected_work = case["expected"][workload_manifest["diagnostic_work"]]
    initial_database = _validate_database_fingerprint(
        value["initial_database"],
        "diagnostic report.initial_database",
    )
    if initial_database != _expected_initial_database(
        workload_manifest,
        case,
    ):
        raise HarnessError(
            "diagnostic report initial database does not match the fixture"
        )
    work = _validate_report_work(
        value["work"],
        expected=expected_work,
        label="diagnostic report work",
    )
    _validate_final_database_state(
        case=case,
        work=work,
        initial=initial_database,
        label="diagnostic report work",
    )
    _validate_effective_configuration(
        value["effective_configuration"],
        engine=expected_engine,
        profile=expected_profile,
    )

    completion = value["completion"]
    _require_type(completion, dict, "diagnostic report.completion")
    _require_keys(
        completion,
        _DIAGNOSTIC_COMPLETION_KEYS,
        "diagnostic report.completion",
    )
    for key in _DIAGNOSTIC_COMPLETION_KEYS - {"status"}:
        _require_type(
            completion[key],
            int,
            f"diagnostic report.completion.{key}",
        )
    expected_completion = {
        "diagnostic_runs": 1,
        "fresh_databases": 2,
        "post_verifications": 2,
        "pre_verifications": 1,
        "status": "complete",
        "warmups": 1,
    }
    if completion != expected_completion:
        raise HarnessError("diagnostic report completion is invalid")

    counters = value["counters"]
    _require_type(counters, dict, "diagnostic report.counters")
    _require_keys(counters, _COUNTER_GROUP_KEYS, "diagnostic report.counters")
    if expected_engine == "modern":
        modern = _validate_counter_values(
            counters["modern"],
            names=MODERN_COUNTER_NAMES,
            label="diagnostic report.counters.modern",
        )
        if modern["allocations"] <= 0:
            raise HarnessError(
                "Modern diagnostic allocations must be positive"
            )
        if modern["vfs_calls"] <= 0:
            raise HarnessError("Modern diagnostic VFS calls must be positive")
        if counters["sqlite"] != {}:
            raise HarnessError("SQLite counters must be empty for Modern")
        _validate_vfs_counters(counters["vfs"])
    else:
        if counters["modern"] != {}:
            raise HarnessError("Modern counters must be empty for SQLite")
        sqlite = _validate_counter_values(
            counters["sqlite"],
            names=SQLITE_COUNTER_NAMES,
            label="diagnostic report.counters.sqlite",
        )
        if sqlite["vm_steps"] <= 0 or sqlite["statement_runs"] <= 0:
            raise HarnessError(
                "SQLite diagnostic execution counters must be positive"
            )
        if counters["vfs"] != {}:
            raise HarnessError("VFS counters must be empty for SQLite")
    return value


@dataclasses.dataclass(frozen=True)
class TimingChildResult:
    command: tuple[str, ...]
    returncode: int
    stdout: bytes
    stderr: bytes
    elapsed_ns: int
    report: dict[str, Any]


@dataclasses.dataclass(frozen=True)
class DiagnosticChildResult:
    command: tuple[str, ...]
    returncode: int
    stdout: bytes
    stderr: bytes
    elapsed_ns: int
    report: dict[str, Any]


def run_timing_child(
    *,
    binary_path: pathlib.Path,
    repository_root: pathlib.Path,
    workload_manifest: dict[str, Any],
    engine: str,
    profile: str,
    case_id: str,
    fixture_path: pathlib.Path,
    scratch_path: pathlib.Path,
    run_kind: str,
    timeout_seconds: float | None = None,
) -> TimingChildResult:
    if engine not in {"modern", "sqlite"}:
        raise HarnessError("timing engine must be modern or sqlite")
    if profile not in {"engine-default", "matched-durable"}:
        raise HarnessError("timing profile is invalid")
    if run_kind not in {"smoke", "baseline"}:
        raise HarnessError("timing run kind must be smoke or baseline")
    if not binary_path.is_file():
        raise HarnessError(f"timing binary does not exist: {binary_path}")
    if not fixture_path.is_file():
        raise HarnessError(f"timing fixture does not exist: {fixture_path}")
    if not scratch_path.is_dir():
        raise HarnessError(
            f"timing scratch path is not a directory: {scratch_path}"
        )
    try:
        scratch_entries = list(scratch_path.iterdir())
    except OSError as error:
        raise HarnessError(f"cannot inspect timing scratch path: {error}") from error
    if scratch_entries:
        raise HarnessError("timing scratch path must be empty")
    _case_by_id(workload_manifest, case_id)
    command = (
        str(binary_path),
        "run",
        engine,
        profile,
        case_id,
        str(fixture_path),
        str(scratch_path),
        run_kind,
    )
    common = _read_performance_module()
    try:
        result = common.run_bounded(
            list(command),
            cwd=repository_root,
            timeout_seconds=(
                timeout_seconds
                if timeout_seconds is not None
                else (600.0 if run_kind == "baseline" else 120.0)
            ),
            stdout_limit=4 * 1024 * 1024,
            stderr_limit=1024 * 1024,
        )
    except common.ChildExecutionError as error:
        raise ChildRunFailure(
            str(error),
            command=command,
            returncode=error.returncode,
            stdout=error.stdout,
            stderr=error.stderr,
            elapsed_ns=error.elapsed_ns,
            timed_out=error.timed_out,
        ) from error
    except common.HarnessError as error:
        raise HarnessError(str(error)) from error
    def fail(message: str) -> None:
        raise ChildRunFailure(
            message,
            command=command,
            returncode=result.returncode,
            stdout=result.stdout,
            stderr=result.stderr,
            elapsed_ns=result.elapsed_ns,
            timed_out=False,
        )

    if result.returncode == 2:
        fail(
            f"timing child reported a correctness mismatch for "
            f"{engine} {profile} {case_id}"
        )
    if result.returncode != 0:
        fail(
            f"timing child failed for {engine} {profile} {case_id} "
            f"with exit {result.returncode}"
        )
    if result.stderr:
        fail(
            f"timing child wrote stderr for {engine} {profile} {case_id}"
        )
    try:
        report = load_json_bytes_strict(
            result.stdout,
            f"timing {engine} {profile} {case_id}",
        )
        validate_raw_timing_report(
            report,
            workload_manifest=workload_manifest,
            expected_engine=engine,
            expected_profile=profile,
            expected_case=case_id,
            expected_run_kind=run_kind,
        )
    except HarnessError as error:
        fail(str(error))
    try:
        remaining = list(scratch_path.iterdir())
    except OSError as error:
        raise HarnessError(f"cannot inspect timing scratch cleanup: {error}") from error
    if remaining:
        fail("timing child left scratch database artifacts")
    return TimingChildResult(
        command=command,
        returncode=result.returncode,
        stdout=result.stdout,
        stderr=result.stderr,
        elapsed_ns=result.elapsed_ns,
        report=report,
    )


def run_diagnostic_child(
    *,
    binary_path: pathlib.Path,
    repository_root: pathlib.Path,
    workload_manifest: dict[str, Any],
    engine: str,
    profile: str,
    case_id: str,
    fixture_path: pathlib.Path,
    scratch_path: pathlib.Path,
    timeout_seconds: float = 600.0,
) -> DiagnosticChildResult:
    if engine not in {"modern", "sqlite"}:
        raise HarnessError("diagnostic engine must be modern or sqlite")
    if profile not in {"engine-default", "matched-durable"}:
        raise HarnessError("diagnostic profile is invalid")
    if not binary_path.is_file():
        raise HarnessError(f"diagnostic binary does not exist: {binary_path}")
    if not fixture_path.is_file():
        raise HarnessError(
            f"diagnostic fixture does not exist: {fixture_path}"
        )
    if not scratch_path.is_dir():
        raise HarnessError(
            f"diagnostic scratch path is not a directory: {scratch_path}"
        )
    try:
        scratch_entries = list(scratch_path.iterdir())
    except OSError as error:
        raise HarnessError(
            f"cannot inspect diagnostic scratch path: {error}"
        ) from error
    if scratch_entries:
        raise HarnessError("diagnostic scratch path must be empty")
    _case_by_id(workload_manifest, case_id)
    command = (
        str(binary_path),
        "run",
        engine,
        profile,
        case_id,
        str(fixture_path),
        str(scratch_path),
        "diagnostic",
    )
    common = _read_performance_module()
    try:
        result = common.run_bounded(
            list(command),
            cwd=repository_root,
            timeout_seconds=timeout_seconds,
            stdout_limit=4 * 1024 * 1024,
            stderr_limit=1024 * 1024,
        )
    except common.ChildExecutionError as error:
        raise ChildRunFailure(
            str(error),
            command=command,
            returncode=error.returncode,
            stdout=error.stdout,
            stderr=error.stderr,
            elapsed_ns=error.elapsed_ns,
            timed_out=error.timed_out,
        ) from error
    except common.HarnessError as error:
        raise HarnessError(str(error)) from error

    def fail(message: str) -> None:
        raise ChildRunFailure(
            message,
            command=command,
            returncode=result.returncode,
            stdout=result.stdout,
            stderr=result.stderr,
            elapsed_ns=result.elapsed_ns,
            timed_out=False,
        )

    if result.returncode == 2:
        fail(
            f"diagnostic child reported a correctness mismatch for "
            f"{engine} {profile} {case_id}"
        )
    if result.returncode != 0:
        fail(
            f"diagnostic child failed for {engine} {profile} {case_id} "
            f"with exit {result.returncode}"
        )
    if result.stderr:
        fail(
            f"diagnostic child wrote stderr for {engine} {profile} {case_id}"
        )
    try:
        report = load_json_bytes_strict(
            result.stdout,
            f"diagnostic {engine} {profile} {case_id}",
        )
        validate_raw_diagnostic_report(
            report,
            workload_manifest=workload_manifest,
            expected_engine=engine,
            expected_profile=profile,
            expected_case=case_id,
        )
    except HarnessError as error:
        fail(str(error))
    try:
        remaining = list(scratch_path.iterdir())
    except OSError as error:
        raise HarnessError(
            f"cannot inspect diagnostic scratch cleanup: {error}"
        ) from error
    if remaining:
        fail("diagnostic child left scratch database artifacts")
    return DiagnosticChildResult(
        command=command,
        returncode=result.returncode,
        stdout=result.stdout,
        stderr=result.stderr,
        elapsed_ns=result.elapsed_ns,
        report=report,
    )


def build_timing_schedule(
    workload_manifest: dict[str, Any],
) -> list[dict[str, Any]]:
    case_ids = [case["id"] for case in workload_manifest["cases"]]
    schedule = []
    ordinal = 0
    for profile in workload_manifest["profiles"]:
        for round_definition in workload_manifest["rounds"]:
            ordered_cases = (
                case_ids
                if round_definition["case_order"] == "canonical"
                else list(reversed(case_ids))
            )
            for case_id in ordered_cases:
                for engine in round_definition["engine_order"]:
                    schedule.append(
                        {
                            "ordinal": ordinal,
                            "profile": profile["id"],
                            "round": round_definition["index"],
                            "case": case_id,
                            "engine": engine,
                        }
                    )
                    ordinal += 1
    return schedule


def build_diagnostic_schedule(
    workload_manifest: dict[str, Any],
) -> list[dict[str, Any]]:
    schedule = []
    ordinal = 0
    for profile in workload_manifest["profiles"]:
        for case in workload_manifest["cases"]:
            for engine in ("modern", "sqlite"):
                schedule.append(
                    {
                        "ordinal": ordinal,
                        "profile": profile["id"],
                        "case": case["id"],
                        "engine": engine,
                    }
                )
                ordinal += 1
    return schedule


def _ratio_within(
    numerator: int,
    denominator: int,
    limit: dict[str, int],
) -> bool:
    if denominator <= 0:
        raise HarnessError("ratio denominator must be positive")
    return (
        numerator * limit["denominator"]
        <= denominator * limit["numerator"]
    )


def _ratio_less(
    left: tuple[int, int],
    right: tuple[int, int],
) -> bool:
    return left[0] * right[1] < right[0] * left[1]


def _aggregate_metric(
    *,
    reports: dict[tuple[str, int, str, str], dict[str, Any]],
    profile: str,
    case_id: str,
    field: str,
) -> dict[str, Any]:
    common = _read_performance_module()
    all_samples: dict[str, list[int]] = {"modern": [], "sqlite": []}
    round_rows = []
    ratio_pairs = []
    for round_index in range(3):
        medians = {}
        for engine in ("modern", "sqlite"):
            report = reports[(profile, round_index, case_id, engine)]
            samples = [row[field] for row in report["repetitions"]]
            all_samples[engine].extend(samples)
            medians[engine] = common.median_integer(samples)
        if medians["sqlite"] <= 0:
            raise HarnessError(
                f"{profile} {case_id} {field} SQLite median is not positive"
            )
        ratio_pair = (medians["modern"], medians["sqlite"])
        ratio_pairs.append(ratio_pair)
        round_rows.append(
            {
                "round": round_index,
                "modern_median_ns": medians["modern"],
                "sqlite_median_ns": medians["sqlite"],
                "ratio": common.exact_ratio(*ratio_pair),
            }
        )
    medians = {
        engine: common.median_integer(samples)
        for engine, samples in all_samples.items()
    }
    if medians["sqlite"] <= 0:
        raise HarnessError(
            f"{profile} {case_id} {field} SQLite aggregate median "
            "is not positive"
        )
    minimum = ratio_pairs[0]
    maximum = ratio_pairs[0]
    for candidate in ratio_pairs[1:]:
        if _ratio_less(candidate, minimum):
            minimum = candidate
        if _ratio_less(maximum, candidate):
            maximum = candidate
    return {
        "sample_count": len(all_samples["modern"]),
        "modern_median_ns": medians["modern"],
        "sqlite_median_ns": medians["sqlite"],
        "ratio": common.exact_ratio(
            medians["modern"],
            medians["sqlite"],
        ),
        "round_ratios": round_rows,
        "minimum_round_ratio": common.exact_ratio(*minimum),
        "maximum_round_ratio": common.exact_ratio(*maximum),
    }


def aggregate_timing_reports(
    workload_manifest: dict[str, Any],
    reports: dict[tuple[str, int, str, str], dict[str, Any]],
) -> dict[str, Any]:
    expected_keys = {
        (
            item["profile"],
            item["round"],
            item["case"],
            item["engine"],
        )
        for item in build_timing_schedule(workload_manifest)
    }
    if set(reports) != expected_keys:
        raise HarnessError("timing report matrix is incomplete or duplicated")
    for profile, round_index, case_id, engine in sorted(expected_keys):
        validate_raw_timing_report(
            reports[(profile, round_index, case_id, engine)],
            workload_manifest=workload_manifest,
            expected_engine=engine,
            expected_profile=profile,
            expected_case=case_id,
            expected_run_kind="baseline",
        )

    guard = workload_manifest["guard"]
    guard_passed = True
    profile_rows = []
    for profile_definition in workload_manifest["profiles"]:
        profile_id = profile_definition["id"]
        guarded = profile_definition["guard"]
        case_rows = []
        profile_passed = True
        for case in workload_manifest["cases"]:
            wall = _aggregate_metric(
                reports=reports,
                profile=profile_id,
                case_id=case["id"],
                field="wall_ns",
            )
            cpu = _aggregate_metric(
                reports=reports,
                profile=profile_id,
                case_id=case["id"],
                field="cpu_ns",
            )
            case_passed = True
            if guarded:
                for metric, limit in (
                    (wall, guard["maximum_wall_ratio"]),
                    (cpu, guard["maximum_cpu_ratio"]),
                ):
                    if not _ratio_within(
                        metric["ratio"]["numerator"],
                        metric["ratio"]["denominator"],
                        limit,
                    ):
                        case_passed = False
                    for round_row in metric["round_ratios"]:
                        ratio = round_row["ratio"]
                        if not _ratio_within(
                            ratio["numerator"],
                            ratio["denominator"],
                            limit,
                        ):
                            case_passed = False
            profile_passed = profile_passed and case_passed
            case_rows.append(
                {
                    "id": case["id"],
                    "guard_passed": case_passed if guarded else None,
                    "wall": wall,
                    "cpu": cpu,
                }
            )
        if guarded:
            guard_passed = guard_passed and profile_passed
            status = "passed" if profile_passed else "failed"
        else:
            status = "informational"
        profile_rows.append(
            {
                "id": profile_id,
                "guard": guarded,
                "status": status,
                "cases": case_rows,
            }
        )
    return {
        "schema_version": SCHEMA_VERSION,
        "workload_semantics_version": WORKLOAD_SEMANTICS_VERSION,
        "status": "complete",
        "guard": guard,
        "guard_passed": guard_passed,
        "profiles": profile_rows,
    }


def _repository_input_reference(
    *,
    role: str,
    path: pathlib.Path,
    repository_root: pathlib.Path,
) -> dict[str, Any]:
    common = _read_performance_module()
    relative = common._repository_relative_path(
        path,
        repository_root,
        f"{role} input",
    )
    return {
        "role": role,
        "path": relative,
        "sha256": _sha256(path),
        "size_bytes": path.stat().st_size,
    }


def _binary_reference(
    *,
    path: pathlib.Path,
    identity: dict[str, Any],
    repository_root: pathlib.Path,
) -> dict[str, Any]:
    common = _read_performance_module()
    return {
        "path": common._repository_relative_path(
            path,
            repository_root,
            "benchmark binary",
        ),
        "sha256": _sha256(path),
        "size_bytes": path.stat().st_size,
        "identity": identity,
    }


def _write_child_artifacts(
    *,
    baseline_path: pathlib.Path,
    group: str,
    stem: str,
    stdout: bytes,
    stderr: bytes,
) -> tuple[dict[str, Any], dict[str, Any]]:
    common = _read_performance_module()
    stdout_path = baseline_path / "raw" / group / f"{stem}.json"
    stderr_path = baseline_path / "raw" / group / f"{stem}.stderr"
    common._write_new_bytes(stdout_path, stdout)
    common._write_new_bytes(stderr_path, stderr)
    return (
        common._artifact_reference(stdout_path, baseline_path),
        common._artifact_reference(stderr_path, baseline_path),
    )


def _fixture_path_for_case(
    *,
    repository_root: pathlib.Path,
    workload_manifest: dict[str, Any],
    case_id: str,
    zero_path: pathlib.Path,
) -> pathlib.Path:
    case = _case_by_id(workload_manifest, case_id)
    if case["fixture"] == "zero":
        return zero_path
    fixture = _fixture_by_id(workload_manifest, case["fixture"])
    return repository_root / pathlib.PurePosixPath(fixture["path"])


def generate_baseline(
    *,
    repository_root: pathlib.Path,
    workload_path: pathlib.Path,
    timing_binary_path: pathlib.Path,
    diagnostic_binary_path: pathlib.Path,
    output_path: pathlib.Path,
) -> dict[str, Any]:
    repository_root = repository_root.resolve()
    workload_path = workload_path.resolve()
    timing_binary_path = timing_binary_path.resolve()
    diagnostic_binary_path = diagnostic_binary_path.resolve()
    output_path = output_path.resolve()
    workload_manifest = load_and_validate_workloads(workload_path)
    common = _read_performance_module()
    protected = [
        workload_path,
        timing_binary_path,
        diagnostic_binary_path,
        pathlib.Path(__file__).resolve(),
        repository_root / "benchmarks/write_performance.cpp",
    ]
    common.validate_new_output_path(output_path, protected_paths=protected)
    baseline_relative = common._repository_relative_path(
        output_path,
        repository_root,
        "baseline output",
    )
    source = common._capture_source_state(
        repository_root,
        baseline_relative,
    )
    if not source["clean"]:
        raise HarnessError("baseline generation requires a clean worktree")
    timing_identity = read_binary_identity(
        binary_path=timing_binary_path,
        repository_root=repository_root,
        instrumentation=False,
    )
    diagnostic_identity = read_binary_identity(
        binary_path=diagnostic_binary_path,
        repository_root=repository_root,
        instrumentation=True,
    )
    expected_source = {
        "revision": source["revision"],
        "tree": source["tree"],
    }
    if (
        timing_identity["source"] != expected_source
        or diagnostic_identity["source"] != expected_source
    ):
        raise HarnessError(
            "benchmark binary source identity does not match the clean tree"
        )
    timing_build = dict(timing_identity["build"])
    diagnostic_build = dict(diagnostic_identity["build"])
    timing_build.pop("instrumentation")
    diagnostic_build.pop("instrumentation")
    if (
        timing_build != diagnostic_build
        or timing_identity["sqlite"] != diagnostic_identity["sqlite"]
    ):
        raise HarnessError(
            "timing and diagnostic binary identities do not match"
        )

    try:
        output_path.mkdir()
        for group in ("timing", "diagnostic"):
            (output_path / "raw" / group).mkdir(parents=True)
        scratch_root = output_path / "scratch"
        scratch_root.mkdir()
        zero_path = scratch_root / "zero-input.db"
        zero_path.write_bytes(b"")
    except OSError as error:
        raise HarnessError(
            f"cannot initialize write baseline output: {error}"
        ) from error

    timing_reports: dict[
        tuple[str, int, str, str],
        dict[str, Any],
    ] = {}
    timing_records = []
    diagnostic_records = []
    try:
        for item in build_timing_schedule(workload_manifest):
            stem = (
                f"{item['ordinal']:03d}-round-{item['round']}-"
                f"{item['profile']}-{item['case']}-{item['engine']}"
            )
            scratch = scratch_root / f"timing-{item['ordinal']:03d}"
            scratch.mkdir()
            fixture_path = _fixture_path_for_case(
                repository_root=repository_root,
                workload_manifest=workload_manifest,
                case_id=item["case"],
                zero_path=zero_path,
            )
            try:
                result = run_timing_child(
                    binary_path=timing_binary_path,
                    repository_root=repository_root,
                    workload_manifest=workload_manifest,
                    engine=item["engine"],
                    profile=item["profile"],
                    case_id=item["case"],
                    fixture_path=fixture_path,
                    scratch_path=scratch,
                    run_kind="baseline",
                )
            except ChildRunFailure as error:
                _write_child_artifacts(
                    baseline_path=output_path,
                    group="timing",
                    stem=stem,
                    stdout=error.stdout,
                    stderr=error.stderr,
                )
                raise
            stdout_ref, stderr_ref = _write_child_artifacts(
                baseline_path=output_path,
                group="timing",
                stem=stem,
                stdout=result.stdout,
                stderr=result.stderr,
            )
            timing_records.append(
                {
                    **item,
                    "command": [
                        timing_binary_path.name,
                        "run",
                        item["engine"],
                        item["profile"],
                        item["case"],
                        fixture_path.name,
                        "SCRATCH",
                        "baseline",
                    ],
                    "process": {
                        "returncode": result.returncode,
                        "elapsed_ns": result.elapsed_ns,
                    },
                    "stdout": stdout_ref,
                    "stderr": stderr_ref,
                }
            )
            timing_reports[
                (
                    item["profile"],
                    item["round"],
                    item["case"],
                    item["engine"],
                )
            ] = result.report
            scratch.rmdir()

        diagnostic_reports = {}
        for item in build_diagnostic_schedule(workload_manifest):
            stem = (
                f"{item['ordinal']:03d}-{item['profile']}-"
                f"{item['case']}-{item['engine']}"
            )
            scratch = scratch_root / f"diagnostic-{item['ordinal']:03d}"
            scratch.mkdir()
            fixture_path = _fixture_path_for_case(
                repository_root=repository_root,
                workload_manifest=workload_manifest,
                case_id=item["case"],
                zero_path=zero_path,
            )
            try:
                result = run_diagnostic_child(
                    binary_path=diagnostic_binary_path,
                    repository_root=repository_root,
                    workload_manifest=workload_manifest,
                    engine=item["engine"],
                    profile=item["profile"],
                    case_id=item["case"],
                    fixture_path=fixture_path,
                    scratch_path=scratch,
                )
            except ChildRunFailure as error:
                _write_child_artifacts(
                    baseline_path=output_path,
                    group="diagnostic",
                    stem=stem,
                    stdout=error.stdout,
                    stderr=error.stderr,
                )
                raise
            stdout_ref, stderr_ref = _write_child_artifacts(
                baseline_path=output_path,
                group="diagnostic",
                stem=stem,
                stdout=result.stdout,
                stderr=result.stderr,
            )
            diagnostic_records.append(
                {
                    **item,
                    "command": [
                        diagnostic_binary_path.name,
                        "run",
                        item["engine"],
                        item["profile"],
                        item["case"],
                        fixture_path.name,
                        "SCRATCH",
                        "diagnostic",
                    ],
                    "process": {
                        "returncode": result.returncode,
                        "elapsed_ns": result.elapsed_ns,
                    },
                    "stdout": stdout_ref,
                    "stderr": stderr_ref,
                }
            )
            diagnostic_reports[
                (item["profile"], item["case"], item["engine"])
            ] = result.report
            scratch.rmdir()

        expected_diagnostic_keys = {
            (item["profile"], item["case"], item["engine"])
            for item in build_diagnostic_schedule(workload_manifest)
        }
        if set(diagnostic_reports) != expected_diagnostic_keys:
            raise HarnessError(
                "diagnostic report matrix is incomplete or duplicated"
            )
        aggregate = aggregate_timing_reports(
            workload_manifest,
            timing_reports,
        )
        common._write_new_json(output_path / "aggregate.json", aggregate)
        inputs = [
            _repository_input_reference(
                role="workload-manifest",
                path=workload_path,
                repository_root=repository_root,
            ),
            _repository_input_reference(
                role="benchmark-source",
                path=repository_root / "benchmarks/write_performance.cpp",
                repository_root=repository_root,
            ),
            _repository_input_reference(
                role="runner-source",
                path=pathlib.Path(__file__).resolve(),
                repository_root=repository_root,
            ),
        ]
        for fixture in workload_manifest["fixtures"]:
            inputs.append(
                _repository_input_reference(
                    role=f"fixture-{fixture['id']}",
                    path=repository_root
                    / pathlib.PurePosixPath(fixture["path"]),
                    repository_root=repository_root,
                )
            )
            inputs.append(
                _repository_input_reference(
                    role=f"fixture-sql-{fixture['id']}",
                    path=repository_root
                    / pathlib.PurePosixPath(fixture["sql_path"]),
                    repository_root=repository_root,
                )
            )
        run_manifest = {
            "schema_version": SCHEMA_VERSION,
            "status": "complete",
            "source": source,
            "host": common._capture_host_provenance(repository_root),
            "inputs": inputs,
            "binaries": {
                "timing": _binary_reference(
                    path=timing_binary_path,
                    identity=timing_identity,
                    repository_root=repository_root,
                ),
                "diagnostic": _binary_reference(
                    path=diagnostic_binary_path,
                    identity=diagnostic_identity,
                    repository_root=repository_root,
                ),
            },
            "timing_runs": timing_records,
            "diagnostic_runs": diagnostic_records,
            "aggregate": common._artifact_reference(
                output_path / "aggregate.json",
                output_path,
            ),
        }
        common._write_new_json(
            output_path / "run-manifest.json",
            run_manifest,
        )
        zero_path.unlink()
        scratch_root.rmdir()
        return aggregate
    except (HarnessError, OSError) as error:
        failure_path = output_path / "failure.json"
        if not failure_path.exists():
            common._write_new_json(
                failure_path,
                {
                    "schema_version": SCHEMA_VERSION,
                    "status": "failed",
                    "error_kind": type(error).__name__,
                    "message": str(error),
                },
            )
        if isinstance(error, OSError):
            raise HarnessError(
                f"write baseline filesystem operation failed: {error}"
            ) from error
        raise


def _validate_input_reference(
    value: Any,
    *,
    repository_root: pathlib.Path,
    label: str,
) -> pathlib.Path:
    _require_type(value, dict, label)
    _require_keys(value, _INPUT_KEYS, label)
    for key in ("role", "path", "sha256"):
        _require_type(value[key], str, f"{label}.{key}")
    _require_type(value["size_bytes"], int, f"{label}.size_bytes")
    pure = pathlib.PurePosixPath(value["path"])
    if pure.is_absolute() or ".." in pure.parts:
        raise HarnessError(f"{label}.path is not repository-relative")
    path = repository_root.joinpath(*pure.parts)
    if not path.is_file():
        raise HarnessError(f"{label} input does not exist")
    if path.stat().st_size != value["size_bytes"] or _sha256(path) != value["sha256"]:
        raise HarnessError(f"{label} input fingerprint differs")
    return path


def _validate_binary_reference(
    value: Any,
    *,
    instrumentation: bool,
    label: str,
) -> None:
    _require_type(value, dict, label)
    _require_keys(value, _BINARY_REFERENCE_KEYS, label)
    for key in ("path", "sha256"):
        _require_type(value[key], str, f"{label}.{key}")
    if _SHA256.fullmatch(value["sha256"]) is None:
        raise HarnessError(f"{label}.sha256 is invalid")
    _require_type(value["size_bytes"], int, f"{label}.size_bytes")
    if value["size_bytes"] <= 0:
        raise HarnessError(f"{label}.size_bytes must be positive")
    validate_binary_identity(
        value["identity"],
        instrumentation=instrumentation,
    )


def _validate_source_state(
    value: Any,
    *,
    repository_root: pathlib.Path,
    baseline_relative: str,
    verify_current_source: bool,
) -> None:
    _require_type(value, dict, "run manifest.source")
    _require_keys(value, _SOURCE_STATE_KEYS, "run manifest.source")
    for key in ("revision", "tree"):
        _require_type(value[key], str, f"run manifest.source.{key}")
        if _GIT_OBJECT.fullmatch(value[key]) is None:
            raise HarnessError(f"run manifest.source.{key} is invalid")
    if value["clean"] is not True:
        raise HarnessError("run manifest source must be clean")
    empty_status = hashlib.sha256(b"").hexdigest()
    if value["status_sha256"] != empty_status:
        raise HarnessError("clean source status hash is invalid")
    for key in ("status_sha256", "worktree_content_sha256"):
        if _SHA256.fullmatch(value[key]) is None:
            raise HarnessError(f"run manifest.source.{key} is invalid")
    if verify_current_source:
        common = _read_performance_module()
        current = common._capture_source_state(
            repository_root,
            baseline_relative,
        )
        if (
            not current["clean"]
            or current["status_sha256"] != value["status_sha256"]
            or current["worktree_content_sha256"]
            != value["worktree_content_sha256"]
        ):
            raise HarnessError(
                "current source state does not match the write baseline"
            )


def _validate_run_record(
    value: Any,
    *,
    expected: dict[str, Any],
    baseline_path: pathlib.Path,
    workload_manifest: dict[str, Any],
    diagnostic: bool,
    expected_identity: dict[str, Any],
) -> tuple[pathlib.Path, pathlib.Path, dict[str, Any]]:
    common = _read_performance_module()
    label = (
        f"diagnostic_runs[{expected['ordinal']}]"
        if diagnostic
        else f"timing_runs[{expected['ordinal']}]"
    )
    _require_type(value, dict, label)
    _require_keys(
        value,
        _DIAGNOSTIC_RUN_KEYS if diagnostic else _TIMING_RUN_KEYS,
        label,
    )
    for key in ("ordinal", "profile", "case", "engine"):
        if value[key] != expected[key]:
            raise HarnessError(f"{label}.{key} does not match the schedule")
    if not diagnostic and value["round"] != expected["round"]:
        raise HarnessError(f"{label}.round does not match the schedule")
    _require_type(value["command"], list, f"{label}.command")
    if len(value["command"]) != 8 or any(
        not isinstance(item, str) or not item
        for item in value["command"]
    ):
        raise HarnessError(f"{label}.command is invalid")
    process = value["process"]
    _require_type(process, dict, f"{label}.process")
    _require_keys(process, _PROCESS_KEYS, f"{label}.process")
    if process["returncode"] != 0:
        raise HarnessError(f"{label}.process did not succeed")
    _require_type(process["elapsed_ns"], int, f"{label}.process.elapsed_ns")
    if process["elapsed_ns"] <= 0:
        raise HarnessError(f"{label}.process.elapsed_ns must be positive")
    stdout_path = common._validate_artifact_reference(
        value["stdout"],
        baseline_path=baseline_path,
        label=f"{label}.stdout",
        maximum_size=4 * 1024 * 1024,
    )
    stderr_path = common._validate_artifact_reference(
        value["stderr"],
        baseline_path=baseline_path,
        label=f"{label}.stderr",
        maximum_size=1024 * 1024,
    )
    if stderr_path.stat().st_size != 0:
        raise HarnessError(f"{label}.stderr is not empty")
    report = load_json_bytes_strict(
        stdout_path.read_bytes(),
        label,
    )
    if diagnostic:
        validate_raw_diagnostic_report(
            report,
            workload_manifest=workload_manifest,
            expected_engine=expected["engine"],
            expected_profile=expected["profile"],
            expected_case=expected["case"],
        )
    else:
        validate_raw_timing_report(
            report,
            workload_manifest=workload_manifest,
            expected_engine=expected["engine"],
            expected_profile=expected["profile"],
            expected_case=expected["case"],
            expected_run_kind="baseline",
        )
        measured_wall = sum(
            repetition["wall_ns"]
            for repetition in report["repetitions"]
        )
        if process["elapsed_ns"] < measured_wall:
            raise HarnessError(
                f"{label}.process elapsed time is below measured wall time"
            )
    if (
        report["build"] != expected_identity["build"]
        or report["source"] != expected_identity["source"]
        or report["sqlite"] != expected_identity["sqlite"]
    ):
        raise HarnessError(f"{label} identity differs from its binary")
    return stdout_path, stderr_path, report


def validate_baseline_directory(
    *,
    baseline_path: pathlib.Path,
    repository_root: pathlib.Path,
    workload_path: pathlib.Path,
    verify_current_source: bool,
) -> dict[str, Any]:
    baseline_path = baseline_path.resolve()
    repository_root = repository_root.resolve()
    workload_path = workload_path.resolve()
    if not baseline_path.is_dir():
        raise HarnessError(f"write baseline does not exist: {baseline_path}")
    workload_manifest = load_and_validate_workloads(workload_path)
    run_manifest = _load_json(baseline_path / "run-manifest.json")
    aggregate = _load_json(baseline_path / "aggregate.json")
    _require_keys(run_manifest, _RUN_MANIFEST_KEYS, "run manifest")
    if run_manifest["schema_version"] != SCHEMA_VERSION:
        raise HarnessError("run manifest schema_version must be 1")
    if run_manifest["status"] != "complete":
        raise HarnessError("run manifest status is not complete")
    common = _read_performance_module()
    baseline_relative = common._repository_relative_path(
        baseline_path,
        repository_root,
        "write baseline",
    )
    _validate_source_state(
        run_manifest["source"],
        repository_root=repository_root,
        baseline_relative=baseline_relative,
        verify_current_source=verify_current_source,
    )
    host = run_manifest["host"]
    _require_type(host, dict, "run manifest.host")
    _require_keys(host, _HOST_KEYS, "run manifest.host")
    for key in _HOST_KEYS - {"logical_cpu_count"}:
        _require_type(host[key], str, f"run manifest.host.{key}")
    _require_type(
        host["logical_cpu_count"],
        int,
        "run manifest.host.logical_cpu_count",
    )
    if host["logical_cpu_count"] <= 0:
        raise HarnessError("run manifest host CPU count must be positive")
    inputs = run_manifest["inputs"]
    _require_type(inputs, list, "run manifest.inputs")
    roles = []
    for index, value in enumerate(inputs):
        _validate_input_reference(
            value,
            repository_root=repository_root,
            label=f"run manifest.inputs[{index}]",
        )
        roles.append(value["role"])
    expected_roles = {
        "workload-manifest",
        "benchmark-source",
        "runner-source",
        "fixture-schema",
        "fixture-sql-schema",
        "fixture-populated",
        "fixture-sql-populated",
    }
    if set(roles) != expected_roles or len(roles) != len(expected_roles):
        raise HarnessError("run manifest input roles are incomplete or duplicated")
    binaries = run_manifest["binaries"]
    _require_type(binaries, dict, "run manifest.binaries")
    _require_keys(binaries, {"timing", "diagnostic"}, "run manifest.binaries")
    _validate_binary_reference(
        binaries["timing"],
        instrumentation=False,
        label="run manifest.binaries.timing",
    )
    _validate_binary_reference(
        binaries["diagnostic"],
        instrumentation=True,
        label="run manifest.binaries.diagnostic",
    )
    timing_build = dict(binaries["timing"]["identity"]["build"])
    diagnostic_build = dict(binaries["diagnostic"]["identity"]["build"])
    timing_build.pop("instrumentation")
    diagnostic_build.pop("instrumentation")
    if (
        timing_build != diagnostic_build
        or binaries["timing"]["identity"]["source"]
        != binaries["diagnostic"]["identity"]["source"]
        or binaries["timing"]["identity"]["sqlite"]
        != binaries["diagnostic"]["identity"]["sqlite"]
    ):
        raise HarnessError("baseline binary identities differ")
    expected_binary_source = {
        "revision": run_manifest["source"]["revision"],
        "tree": run_manifest["source"]["tree"],
    }
    if binaries["timing"]["identity"]["source"] != expected_binary_source:
        raise HarnessError(
            "baseline binary source does not match the run manifest"
        )

    referenced_paths = {
        baseline_path / "run-manifest.json",
        baseline_path / "aggregate.json",
    }
    timing_records = run_manifest["timing_runs"]
    timing_schedule = build_timing_schedule(workload_manifest)
    _require_type(timing_records, list, "run manifest.timing_runs")
    if len(timing_records) != len(timing_schedule):
        raise HarnessError("run manifest timing run count is invalid")
    timing_reports = {}
    for record, expected in zip(
        timing_records,
        timing_schedule,
        strict=True,
    ):
        stdout_path, stderr_path, report = _validate_run_record(
            record,
            expected=expected,
            baseline_path=baseline_path,
            workload_manifest=workload_manifest,
            diagnostic=False,
            expected_identity=binaries["timing"]["identity"],
        )
        referenced_paths.update((stdout_path, stderr_path))
        timing_reports[
            (
                expected["profile"],
                expected["round"],
                expected["case"],
                expected["engine"],
            )
        ] = report

    diagnostic_records = run_manifest["diagnostic_runs"]
    diagnostic_schedule = build_diagnostic_schedule(workload_manifest)
    _require_type(
        diagnostic_records,
        list,
        "run manifest.diagnostic_runs",
    )
    if len(diagnostic_records) != len(diagnostic_schedule):
        raise HarnessError("run manifest diagnostic run count is invalid")
    for record, expected in zip(
        diagnostic_records,
        diagnostic_schedule,
        strict=True,
    ):
        stdout_path, stderr_path, _ = _validate_run_record(
            record,
            expected=expected,
            baseline_path=baseline_path,
            workload_manifest=workload_manifest,
            diagnostic=True,
            expected_identity=binaries["diagnostic"]["identity"],
        )
        referenced_paths.update((stdout_path, stderr_path))

    aggregate_path = common._validate_artifact_reference(
        run_manifest["aggregate"],
        baseline_path=baseline_path,
        label="run manifest.aggregate",
        maximum_size=4 * 1024 * 1024,
    )
    if aggregate_path != baseline_path / "aggregate.json":
        raise HarnessError("run manifest aggregate path is invalid")
    recomputed = aggregate_timing_reports(
        workload_manifest,
        timing_reports,
    )
    if aggregate != recomputed:
        raise HarnessError("write aggregate does not match raw timing reports")
    actual_paths = {
        path
        for path in baseline_path.rglob("*")
        if path.is_file()
    }
    if actual_paths != referenced_paths:
        raise HarnessError("write baseline contains missing or extra artifacts")
    return aggregate


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as input_file:
            while chunk := input_file.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise HarnessError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _fnv1a64(data: bytes, state: int) -> int:
    for byte in data:
        state ^= byte
        state = (state * _FNV_PRIME) & 0xFFFF_FFFF_FFFF_FFFF
    return state


def _expected_value(rowid: int) -> bytes:
    return f"{rowid:08x}".encode("ascii") + (b"0" * 248)


def _fixture_content_digest(
    common: Any,
    oracle: Any,
    database: Any,
    expected_rows: int,
) -> str:
    statement = common._sqlite_prepare(
        oracle,
        database,
        "SELECT k,v,version FROM kv",
    )
    state = _FNV_OFFSET
    rowid = 1
    try:
        while True:
            result = oracle.library.sqlite3_step(statement)
            if result == _SQLITE_DONE:
                break
            if result != _SQLITE_ROW:
                raise HarnessError(
                    f"write fixture scan failed with SQLite code {result}"
                )
            actual_rowid = int(
                oracle.library.sqlite3_column_int64(statement, 0)
            )
            pointer = oracle.library.sqlite3_column_blob(statement, 1)
            length = oracle.library.sqlite3_column_bytes(statement, 1)
            version = int(oracle.library.sqlite3_column_int64(statement, 2))
            if pointer is None or length != 256:
                raise HarnessError("write fixture row contains an invalid BLOB")
            value = ctypes.string_at(pointer, length)
            if (
                actual_rowid != rowid
                or value != _expected_value(rowid)
                or version != 0
            ):
                raise HarnessError(
                    f"write fixture row {rowid} is not canonical"
                )
            state = _fnv1a64(actual_rowid.to_bytes(8, "little"), state)
            state = _fnv1a64(value, state)
            state = _fnv1a64(version.to_bytes(8, "little"), state)
            rowid += 1
    finally:
        common._sqlite_finalize(oracle, statement)
    if rowid - 1 != expected_rows:
        raise HarnessError(
            "write fixture row count mismatch: "
            f"expected {expected_rows}, got {rowid - 1}"
        )
    return f"{state:016x}"


def create_fixture(
    *,
    profile_path: pathlib.Path,
    sqlite_library_path: pathlib.Path,
    sqlite_c_path: pathlib.Path,
    sqlite_h_path: pathlib.Path,
    sql_path: pathlib.Path,
    fixture_id: str,
    output_path: pathlib.Path,
) -> dict[str, Any]:
    if fixture_id not in {"schema", "populated"}:
        raise HarnessError("fixture ID must be schema or populated")
    common = _read_performance_module()
    inputs = [
        profile_path,
        sqlite_library_path,
        sqlite_c_path,
        sqlite_h_path,
        sql_path,
        pathlib.Path(__file__),
        pathlib.Path(common.__file__),
    ]
    for path in inputs:
        if not path.is_file():
            raise HarnessError(f"fixture input is not a file: {path}")
    common.validate_new_output_path(output_path, protected_paths=inputs)
    profile = common._validate_profile(
        common.load_json_strict(profile_path),
        label="SQLite profile",
    )
    try:
        sql = sql_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise HarnessError(f"cannot read fixture SQL {sql_path}: {error}") from error
    if not sql or "\0" in sql:
        raise HarnessError("fixture SQL must be nonempty UTF-8 without NUL bytes")

    sidecars = [
        pathlib.Path(f"{output_path}-journal"),
        pathlib.Path(f"{output_path}-wal"),
        pathlib.Path(f"{output_path}-shm"),
    ]
    if any(os.path.lexists(path) for path in sidecars):
        raise HarnessError("fixture sidecar paths already exist")

    oracle = common._load_pinned_sqlite(
        library_path=sqlite_library_path,
        profile=profile,
        sqlite_c_path=sqlite_c_path,
        sqlite_h_path=sqlite_h_path,
    )
    try:
        oracle.create_database(output_path, sql)
    except Exception as error:
        if isinstance(error, common.HarnessError):
            raise HarnessError(str(error)) from error
        raise
    if any(os.path.lexists(path) for path in sidecars):
        raise HarnessError("fixture generation left a sidecar")

    try:
        header = output_path.read_bytes()[:100]
        size_bytes = output_path.stat().st_size
    except OSError as error:
        raise HarnessError(f"cannot inspect generated fixture: {error}") from error
    if len(header) != 100 or header[:16] != b"SQLite format 3\0":
        raise HarnessError("generated fixture does not have a SQLite header")
    encoded_page_size = int.from_bytes(header[16:18], "big")
    page_size = 65_536 if encoded_page_size == 1 else encoded_page_size
    if page_size != 4096 or size_bytes % page_size != 0:
        raise HarnessError("generated fixture has an invalid page layout")
    page_count = size_bytes // page_size
    if int.from_bytes(header[28:32], "big") != page_count:
        raise HarnessError("generated fixture header page count is invalid")

    database, result = oracle._open(output_path, _SQLITE_OPEN_READONLY)
    if result != _SQLITE_OK:
        try:
            oracle._close(database)
        finally:
            raise HarnessError(
                f"cannot reopen generated write fixture: SQLite code {result}"
            )
    try:
        oracle._configure_connection(database)
        integrity = common._sqlite_single_text(
            oracle,
            database,
            "PRAGMA integrity_check",
        )
        schema_sql = common._sqlite_single_text(
            oracle,
            database,
            "SELECT sql FROM sqlite_schema "
            "WHERE type='table' AND name='kv'",
        )
        column_signature = common._sqlite_single_text(
            oracle,
            database,
            "SELECT group_concat(signature, '|') FROM ("
            "SELECT cid||':'||name||':'||type||':'||\"notnull\"||':'||"
            "coalesce(dflt_value,'NULL')||':'||pk||':'||hidden AS signature "
            "FROM pragma_table_xinfo('kv') ORDER BY cid)",
        )
        row_count = common._sqlite_single_integer(
            oracle,
            database,
            "SELECT count(*) FROM kv",
        )
        expected_rows = 0 if fixture_id == "schema" else 65_536
        if (
            integrity != "ok"
            or schema_sql
            != "CREATE TABLE kv(\n"
            "  k INTEGER PRIMARY KEY,\n"
            "  v BLOB NOT NULL,\n"
            "  version INTEGER NOT NULL\n"
            ")"
            or column_signature
            != "0:k:INTEGER:0:NULL:1:0|"
            "1:v:BLOB:1:NULL:0:0|"
            "2:version:INTEGER:1:NULL:0:0"
            or common._sqlite_single_integer(
                oracle,
                database,
                "SELECT count(*) FROM sqlite_schema "
                "WHERE name NOT LIKE 'sqlite_%'",
            )
            != 1
            or common._sqlite_single_text(
                oracle,
                database,
                "PRAGMA encoding",
            )
            != "UTF-8"
            or common._sqlite_single_integer(
                oracle,
                database,
                "PRAGMA auto_vacuum",
            )
            != 0
            or common._sqlite_single_integer(
                oracle,
                database,
                "PRAGMA application_id",
            )
            != 0
            or common._sqlite_single_integer(
                oracle,
                database,
                "PRAGMA user_version",
            )
            != 1
            or common._sqlite_single_integer(
                oracle,
                database,
                "PRAGMA page_size",
            )
            != page_size
            or common._sqlite_single_integer(
                oracle,
                database,
                "PRAGMA page_count",
            )
            != page_count
            or common._sqlite_single_text(
                oracle,
                database,
                "PRAGMA journal_mode",
            ).lower()
            != "delete"
            or row_count != expected_rows
        ):
            raise HarnessError("generated write fixture contract is not canonical")
        content_digest = _fixture_content_digest(
            common,
            oracle,
            database,
            expected_rows,
        )
        freelist_count = common._sqlite_single_integer(
            oracle,
            database,
            "PRAGMA freelist_count",
        )
        schema_cookie = common._sqlite_single_integer(
            oracle,
            database,
            "PRAGMA schema_version",
        )
        if freelist_count != 0 or schema_cookie != 1:
            raise HarnessError(
                "generated write fixture header state is not canonical"
            )
    finally:
        oracle._close(database)

    if fixture_id == "schema" and page_count >= 512:
        raise HarnessError("schema fixture does not fit in the 512-page cache")
    if fixture_id == "populated" and page_count <= 512:
        raise HarnessError("populated fixture does not exceed the 512-page cache")
    return {
        "id": fixture_id,
        "path": output_path.name,
        "sql_path": sql_path.name,
        "sha256": _sha256(output_path),
        "sql_sha256": _sha256(sql_path),
        "size_bytes": size_bytes,
        "page_size": page_size,
        "page_count": page_count,
        "row_count": row_count,
        "value_size": 256,
        "content_digest": content_digest,
        "freelist_count": freelist_count,
        "schema_cookie": schema_cookie,
    }


def _parse_arguments() -> argparse.Namespace:
    parser = _ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate = subparsers.add_parser("validate-workloads")
    validate.add_argument("--workloads", required=True, type=pathlib.Path)
    regenerate = subparsers.add_parser("regenerate-fixture")
    regenerate.add_argument("--profile", required=True, type=pathlib.Path)
    regenerate.add_argument(
        "--sqlite-library", required=True, type=pathlib.Path
    )
    regenerate.add_argument("--sqlite-c", required=True, type=pathlib.Path)
    regenerate.add_argument("--sqlite-h", required=True, type=pathlib.Path)
    regenerate.add_argument("--sql", required=True, type=pathlib.Path)
    regenerate.add_argument(
        "--fixture-id",
        required=True,
        choices=("schema", "populated"),
    )
    regenerate.add_argument("--output", required=True, type=pathlib.Path)
    generate = subparsers.add_parser("generate-baseline")
    generate.add_argument(
        "--repository-root",
        required=True,
        type=pathlib.Path,
    )
    generate.add_argument("--workloads", required=True, type=pathlib.Path)
    generate.add_argument(
        "--timing-binary",
        required=True,
        type=pathlib.Path,
    )
    generate.add_argument(
        "--diagnostic-binary",
        required=True,
        type=pathlib.Path,
    )
    generate.add_argument("--output", required=True, type=pathlib.Path)
    validate_baseline = subparsers.add_parser("validate-baseline")
    validate_baseline.add_argument(
        "--repository-root",
        required=True,
        type=pathlib.Path,
    )
    validate_baseline.add_argument(
        "--workloads",
        required=True,
        type=pathlib.Path,
    )
    validate_baseline.add_argument(
        "--baseline",
        required=True,
        type=pathlib.Path,
    )
    return parser.parse_args()


def main() -> int:
    try:
        arguments = _parse_arguments()
        if arguments.command == "generate-baseline":
            aggregate = generate_baseline(
                repository_root=arguments.repository_root,
                workload_path=arguments.workloads,
                timing_binary_path=arguments.timing_binary,
                diagnostic_binary_path=arguments.diagnostic_binary,
                output_path=arguments.output,
            )
            print(json.dumps(aggregate, sort_keys=True))
            return 0 if aggregate["guard_passed"] else 2
        if arguments.command == "validate-baseline":
            aggregate = validate_baseline_directory(
                baseline_path=arguments.baseline,
                repository_root=arguments.repository_root,
                workload_path=arguments.workloads,
                verify_current_source=True,
            )
            print(
                "validated write baseline with "
                f"{len(aggregate['profiles'])} profiles"
            )
            return 0
        if arguments.command == "regenerate-fixture":
            metadata = create_fixture(
                profile_path=arguments.profile.resolve(),
                sqlite_library_path=arguments.sqlite_library.resolve(),
                sqlite_c_path=arguments.sqlite_c.resolve(),
                sqlite_h_path=arguments.sqlite_h.resolve(),
                sql_path=arguments.sql.resolve(),
                fixture_id=arguments.fixture_id,
                output_path=arguments.output.resolve(),
            )
            print(json.dumps(metadata, sort_keys=True))
            return 0
        if arguments.command == "validate-workloads":
            cases, profiles = validate_workloads(arguments.workloads)
            print(
                f"validated {cases} write performance cases and "
                f"{profiles} profiles"
            )
            return 0
        raise HarnessError("unknown command")
    except HarnessError as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
