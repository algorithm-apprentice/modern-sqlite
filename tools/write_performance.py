#!/usr/bin/env python3

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import pathlib
import re
import sys
from typing import Any


class HarnessError(RuntimeError):
    pass


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise HarnessError(f"{self.prog}: error: {message}")


SCHEMA_VERSION = 1
WORKLOAD_SEMANTICS_VERSION = 1
SQLITE_PROFILE = "sqlite-oracle-profile-v1"
MINIMUM_WALL_NS = 20_000_000
TIMING_REPETITIONS = 3

_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_DIGEST = re.compile(r"[0-9a-f]{16}\Z")

_TOP_KEYS = {
    "schema_version",
    "workload_semantics_version",
    "sqlite_profile",
    "configuration",
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
}
_CASE_KEYS = {
    "id",
    "kind",
    "fixture",
    "primary_unit",
    "sql",
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
            parse_constant=lambda token: (_ for _ in ()).throw(
                HarnessError(f"non-finite JSON number is not allowed: {token}")
            ),
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise HarnessError(f"could not parse workload manifest: {error}") from error
    _require_type(value, dict, "workload manifest")
    return value


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
    _validate_configuration(value["configuration"])
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
    return parser.parse_args()


def main() -> int:
    try:
        arguments = _parse_arguments()
        if arguments.command != "validate-workloads":
            if arguments.command != "regenerate-fixture":
                raise HarnessError("unknown command")
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
        else:
            cases, profiles = validate_workloads(arguments.workloads)
            print(
                f"validated {cases} write performance cases and "
                f"{profiles} profiles"
            )
        return 0
    except HarnessError as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
