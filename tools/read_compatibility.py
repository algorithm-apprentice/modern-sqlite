#!/usr/bin/env python3

import argparse
import ctypes
import hashlib
import json
import os
import pathlib
import re
import selectors
import shlex
import struct
import subprocess
import sys
import tempfile
import time
from typing import Any


class HarnessError(RuntimeError):
    pass


class VerificationMismatch(RuntimeError):
    def __init__(
        self,
        classification: str,
        case: dict[str, Any],
        expected: Any,
        actual: Any,
        difference: str,
        protocol: bytes,
        runner: pathlib.Path,
        database: pathlib.Path,
    ) -> None:
        super().__init__(classification)
        self.classification = classification
        self.case = case
        self.expected = expected
        self.actual = actual
        self.difference = difference
        self.protocol = protocol
        self.runner = runner
        self.database = database

    def render(self) -> str:
        command = (
            f"printf %s {shlex.quote(self.protocol.decode('ascii'))} | "
            f"{shlex.quote(str(self.runner))} {shlex.quote(str(self.database))}"
        )
        return "\n".join(
            [
                f"{self.classification}: {self.case['id']}",
                f"feature: {self.case['feature']}",
                f"database: {self.database}",
                f"sql_hex: {self.case['sql_hex']}",
                "operations: "
                + json.dumps(
                    self.case["operations"],
                    sort_keys=True,
                    separators=(",", ":"),
                ),
                "expected: "
                + json.dumps(self.expected, sort_keys=True, separators=(",", ":")),
                "actual: "
                + json.dumps(self.actual, sort_keys=True, separators=(",", ":")),
                f"first difference: {self.difference}",
                "reproduce:",
                f"  {command}",
                "",
            ]
        )


CORPUS_LIMITS = {
    "corpus_bytes": 4_194_304,
    "oracle_bytes": 67_108_864,
    "cases": 512,
    "sql_bytes": 65_536,
    "operations": 1_024,
    "binding_operations": 256,
    "result_columns": 256,
    "result_rows": 4_096,
    "value_bytes": 1_048_576,
    "output_bytes": 16_777_216,
    "stderr_bytes": 1_048_576,
    "child_seconds": 10,
}

PROFILE_DB_CONFIG = {
    "DQS_DML": 1,
    "DQS_DDL": 1,
    "REVERSE_SCANORDER": 0,
    "ENABLE_COMMENTS": 1,
    "FP_DIGITS": 17,
}

PROFILE_LIMITS = {
    "LENGTH": 1_048_576,
    "SQL_LENGTH": 65_536,
    "COLUMN": 256,
    "EXPR_DEPTH": 1_000,
    "VDBE_OP": 25_000,
    "FUNCTION_ARG": 1_000,
    "VARIABLE_NUMBER": 256,
}

SQLITE_BIND_INDEX_MAX = 2_147_483_647
_DIAGNOSTIC_COMPILE_OPTION_PREFIXES = ("COMPILER=",)

ERROR_CODES = {
    "generic",
    "internal",
    "permission_denied",
    "aborted",
    "busy",
    "locked",
    "out_of_memory",
    "read_only",
    "interrupted",
    "io",
    "corruption",
    "not_found",
    "full",
    "cannot_open",
    "protocol",
    "empty",
    "schema_changed",
    "too_large",
    "constraint",
    "type_mismatch",
    "misuse",
    "no_large_file_support",
    "authorization",
    "format",
    "out_of_range",
    "not_database",
}

_IDENTIFIER = re.compile(r"[a-z][a-z0-9_-]*\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_LOWER_HEX = re.compile(r"(?:[0-9a-f]{2})*\Z")
_REAL_BITS = re.compile(r"[0-9a-f]{16}\Z")
_INTEGER = re.compile(r"(?:0|-[1-9][0-9]*|[1-9][0-9]*)\Z")

_SQLITE_OPEN_READONLY = 0x00000001
_SQLITE_OPEN_READWRITE = 0x00000002
_SQLITE_OPEN_CREATE = 0x00000004
_SQLITE_OK = 0
_SQLITE_ROW = 100
_SQLITE_DONE = 101
_SQLITE_NOMEM = 7
_SQLITE_NULL = 5
_SQLITE_INTEGER = 1
_SQLITE_FLOAT = 2
_SQLITE_TEXT = 3
_SQLITE_BLOB = 4
_SQLITE_TRANSIENT = ctypes.c_void_p(-1)

_DB_CONFIG_IDS = {
    "DQS_DML": 1013,
    "DQS_DDL": 1014,
    "REVERSE_SCANORDER": 1019,
    "ENABLE_COMMENTS": 1022,
    "FP_DIGITS": 1023,
}

_LIMIT_IDS = {
    "LENGTH": 0,
    "SQL_LENGTH": 1,
    "COLUMN": 2,
    "EXPR_DEPTH": 3,
    "VDBE_OP": 5,
    "FUNCTION_ARG": 6,
    "VARIABLE_NUMBER": 9,
}

_PRIMARY_CODE_NAMES = {
    0: "ok",
    1: "generic",
    2: "internal",
    3: "permission_denied",
    4: "aborted",
    5: "busy",
    6: "locked",
    7: "out_of_memory",
    8: "read_only",
    9: "interrupted",
    10: "io",
    11: "corruption",
    12: "not_found",
    13: "full",
    14: "cannot_open",
    15: "protocol",
    16: "empty",
    17: "schema_changed",
    18: "too_large",
    19: "constraint",
    20: "type_mismatch",
    21: "misuse",
    22: "no_large_file_support",
    23: "authorization",
    24: "format",
    25: "out_of_range",
    26: "not_database",
}


def _duplicate_rejecting_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise HarnessError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def _reject_nonfinite(token: str) -> None:
    raise HarnessError(f"non-finite JSON number is not allowed: {token}")


def load_json_strict(path: pathlib.Path) -> Any:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise HarnessError(f"cannot read JSON file {path}: {error}") from error
    try:
        return json.loads(
            text,
            object_pairs_hook=_duplicate_rejecting_object,
            parse_constant=_reject_nonfinite,
        )
    except HarnessError:
        raise
    except json.JSONDecodeError as error:
        raise HarnessError(f"invalid JSON in {path}: {error}") from error


def _load_json_bytes_strict(data: bytes, label: str) -> Any:
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


def _require_type(value: Any, expected: type, label: str) -> None:
    if type(value) is not expected:
        raise HarnessError(f"{label} must be {expected.__name__}")


def _require_keys(
    value: Any, required: set[str], label: str, optional: set[str] | None = None
) -> dict[str, Any]:
    _require_type(value, dict, label)
    optional = optional or set()
    actual = set(value)
    missing = required - actual
    extra = actual - required - optional
    if missing:
        raise HarnessError(f"{label} is missing keys: {', '.join(sorted(missing))}")
    if extra:
        raise HarnessError(f"{label} has unknown keys: {', '.join(sorted(extra))}")
    return value


def _require_string(value: Any, label: str, *, nonempty: bool = True) -> str:
    _require_type(value, str, label)
    if nonempty and not value:
        raise HarnessError(f"{label} must not be empty")
    return value


def _require_integer(value: Any, label: str, minimum: int = 0) -> int:
    _require_type(value, int, label)
    if value < minimum:
        raise HarnessError(f"{label} must be at least {minimum}")
    return value


def _require_sha256(value: Any, label: str) -> str:
    text = _require_string(value, label)
    if _SHA256.fullmatch(text) is None:
        raise HarnessError(f"{label} must be a lowercase SHA-256 digest")
    return text


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise HarnessError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _repository_path(
    repository_root: pathlib.Path, relative: Any, label: str
) -> pathlib.Path:
    text = _require_string(relative, label)
    pure = pathlib.PurePosixPath(text)
    if pure.is_absolute() or ".." in pure.parts or "." in pure.parts:
        raise HarnessError(f"{label} must be a normalized repository-relative path")
    root = repository_root.resolve()
    path = (root / pathlib.Path(*pure.parts)).resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise HarnessError(f"{label} escapes the repository root") from error
    return path


def _require_string_list(value: Any, label: str) -> list[str]:
    _require_type(value, list, label)
    result: list[str] = []
    for index, item in enumerate(value):
        result.append(_require_string(item, f"{label}[{index}]"))
    if len(result) != len(set(result)):
        raise HarnessError(f"{label} contains duplicates")
    return result


def _validate_profile_sqlite(value: Any) -> dict[str, Any]:
    sqlite = _require_keys(
        value,
        {
            "version",
            "source_id",
            "fossil_check_in",
            "git_mirror_commit",
            "sqlite3_c_sha256",
            "sqlite3_h_sha256",
        },
        "profile.sqlite",
    )
    if _require_string(sqlite["version"], "profile.sqlite.version") != "3.54.0":
        raise HarnessError("profile.sqlite.version must be 3.54.0")
    source_id = _require_string(sqlite["source_id"], "profile.sqlite.source_id")
    fossil = _require_string(
        sqlite["fossil_check_in"], "profile.sqlite.fossil_check_in"
    )
    if len(fossil) != 64 or fossil not in source_id:
        raise HarnessError("profile SQLite source ID does not contain the Fossil check-in")
    mirror = _require_string(
        sqlite["git_mirror_commit"], "profile.sqlite.git_mirror_commit"
    )
    if re.fullmatch(r"[0-9a-f]{40}", mirror) is None:
        raise HarnessError("profile.sqlite.git_mirror_commit must be a Git SHA-1")
    _require_sha256(sqlite["sqlite3_c_sha256"], "profile.sqlite.sqlite3_c_sha256")
    _require_sha256(sqlite["sqlite3_h_sha256"], "profile.sqlite.sqlite3_h_sha256")
    return sqlite


def _validate_profile_build(value: Any) -> dict[str, Any]:
    build = _require_keys(
        value,
        {
            "defines",
            "required_compile_options",
            "forbidden_compile_options",
            "semantic_compile_options",
            "commands",
        },
        "profile.build",
    )
    defines = _require_string_list(build["defines"], "profile.build.defines")
    for required in ("NDEBUG", "SQLITE_DQS=3", "SQLITE_THREADSAFE=0"):
        if required not in defines:
            raise HarnessError(f"profile.build.defines is missing {required}")
    required_options = _require_string_list(
        build["required_compile_options"],
        "profile.build.required_compile_options",
    )
    for required in ("DQS=3", "THREADSAFE=0"):
        if required not in required_options:
            raise HarnessError(
                f"profile.build.required_compile_options is missing {required}"
            )
    forbidden_options = _require_string_list(
        build["forbidden_compile_options"],
        "profile.build.forbidden_compile_options",
    )
    for forbidden in ("OMIT_DECLTYPE", "OMIT_FLOATING_POINT"):
        if forbidden not in forbidden_options:
            raise HarnessError(
                f"profile.build.forbidden_compile_options is missing {forbidden}"
            )
    semantic_options = _require_string_list(
        build["semantic_compile_options"],
        "profile.build.semantic_compile_options",
    )
    if semantic_options != sorted(semantic_options):
        raise HarnessError(
            "profile.build.semantic_compile_options must be sorted"
        )
    for option in semantic_options:
        if option.startswith(_DIAGNOSTIC_COMPILE_OPTION_PREFIXES):
            raise HarnessError(
                "profile.build.semantic_compile_options contains "
                "a diagnostic-only option"
            )
    for required in required_options:
        if required not in semantic_options:
            raise HarnessError(
                "profile.build.semantic_compile_options is missing "
                f"{required}"
            )
    for forbidden in forbidden_options:
        if any(
            option == forbidden or option.startswith(f"{forbidden}=")
            for option in semantic_options
        ):
            raise HarnessError(
                "profile.build.semantic_compile_options contains "
                f"{forbidden}"
            )
    commands = _require_keys(
        build["commands"], {"linux", "macos"}, "profile.build.commands"
    )
    _require_string(commands["linux"], "profile.build.commands.linux")
    _require_string(commands["macos"], "profile.build.commands.macos")
    return build


def _validate_profile_connection(value: Any) -> dict[str, Any]:
    connection = _require_keys(
        value,
        {"db_config", "extended_result_codes", "limits"},
        "profile.connection",
    )
    db_config = _require_keys(
        connection["db_config"], set(PROFILE_DB_CONFIG), "profile.connection.db_config"
    )
    for name, expected in PROFILE_DB_CONFIG.items():
        if _require_integer(
            db_config[name], f"profile.connection.db_config.{name}"
        ) != expected:
            raise HarnessError(
                f"profile.connection.db_config.{name} must be {expected}"
            )
    _require_type(
        connection["extended_result_codes"],
        bool,
        "profile.connection.extended_result_codes",
    )
    if connection["extended_result_codes"]:
        raise HarnessError("profile.connection.extended_result_codes must be false")
    limits = _require_keys(
        connection["limits"], set(PROFILE_LIMITS), "profile.connection.limits"
    )
    for name, expected in PROFILE_LIMITS.items():
        if _require_integer(
            limits[name], f"profile.connection.limits.{name}"
        ) != expected:
            raise HarnessError(f"profile.connection.limits.{name} must be {expected}")
    return connection


def validate_profile(
    repository_root: pathlib.Path, profile_path: pathlib.Path
) -> dict[str, Any]:
    try:
        size = profile_path.stat().st_size
    except OSError as error:
        raise HarnessError(f"cannot stat oracle profile {profile_path}: {error}") from error
    if size > CORPUS_LIMITS["corpus_bytes"]:
        raise HarnessError("oracle profile exceeds the file-size limit")

    profile = _require_keys(
        load_json_strict(profile_path),
        {
            "schema_version",
            "sqlite",
            "build",
            "connection",
            "generator",
            "fixture_sources",
        },
        "profile",
    )
    if _require_integer(profile["schema_version"], "profile.schema_version") != 1:
        raise HarnessError("profile.schema_version must be 1")
    _validate_profile_sqlite(profile["sqlite"])
    _validate_profile_build(profile["build"])
    _validate_profile_connection(profile["connection"])

    generator = _require_keys(
        profile["generator"], {"path", "sha256"}, "profile.generator"
    )
    generator_path = _repository_path(
        repository_root, generator["path"], "profile.generator.path"
    )
    expected_generator_hash = _require_sha256(
        generator["sha256"], "profile.generator.sha256"
    )
    if _sha256(generator_path) != expected_generator_hash:
        raise HarnessError("generator hash mismatch")

    fixture_sources = profile["fixture_sources"]
    _require_type(fixture_sources, list, "profile.fixture_sources")
    if not fixture_sources:
        raise HarnessError("profile.fixture_sources must not be empty")
    source_paths: list[pathlib.Path] = []
    seen_paths: set[str] = set()
    for index, item in enumerate(fixture_sources):
        source = _require_keys(
            item, {"path", "sha256"}, f"profile.fixture_sources[{index}]"
        )
        relative = _require_string(
            source["path"], f"profile.fixture_sources[{index}].path"
        )
        if relative in seen_paths:
            raise HarnessError(f"duplicate fixture source path: {relative}")
        seen_paths.add(relative)
        path = _repository_path(
            repository_root,
            relative,
            f"profile.fixture_sources[{index}].path",
        )
        expected_hash = _require_sha256(
            source["sha256"], f"profile.fixture_sources[{index}].sha256"
        )
        if _sha256(path) != expected_hash:
            raise HarnessError(f"fixture source hash mismatch: {relative}")
        source_paths.append(path)

    normalized = dict(profile)
    normalized["generator_path"] = generator_path
    normalized["fixture_source_paths"] = source_paths
    return normalized


def _validate_value(value: Any, case_id: str) -> None:
    typed = _require_keys(
        value, {"type"}, f"case {case_id}: binding value", {"value", "bits", "hex"}
    )
    value_type = _require_string(
        typed["type"], f"case {case_id}: binding value type"
    )
    if value_type == "null":
        _require_keys(typed, {"type"}, f"case {case_id}: NULL value")
        return
    if value_type == "integer":
        _require_keys(typed, {"type", "value"}, f"case {case_id}: INTEGER value")
        integer = _require_string(
            typed["value"], f"case {case_id}: INTEGER value"
        )
        if _INTEGER.fullmatch(integer) is None:
            raise HarnessError(
                f"case {case_id}: INTEGER value must be canonical signed decimal"
            )
        parsed = int(integer)
        if parsed < -(1 << 63) or parsed > (1 << 63) - 1:
            raise HarnessError(f"case {case_id}: INTEGER value is out of range")
        return
    if value_type == "real":
        _require_keys(typed, {"type", "bits"}, f"case {case_id}: REAL value")
        bits = _require_string(typed["bits"], f"case {case_id}: REAL bits")
        if _REAL_BITS.fullmatch(bits) is None:
            raise HarnessError(
                f"case {case_id}: REAL bits must contain 16 hexadecimal digits"
            )
        return
    if value_type in {"text", "blob"}:
        label = value_type.upper()
        _require_keys(typed, {"type", "hex"}, f"case {case_id}: {label} value")
        encoded = _require_string(
            typed["hex"], f"case {case_id}: {label} bytes", nonempty=False
        )
        if _LOWER_HEX.fullmatch(encoded) is None:
            raise HarnessError(
                f"case {case_id}: {label} bytes must be lowercase hexadecimal"
            )
        if len(encoded) // 2 > CORPUS_LIMITS["value_bytes"]:
            raise HarnessError(f"case {case_id}: {label} value exceeds the byte limit")
        if value_type == "text":
            try:
                bytes.fromhex(encoded).decode("utf-8")
            except UnicodeDecodeError as error:
                raise HarnessError(
                    f"case {case_id}: TEXT bytes are not valid UTF-8"
                ) from error
        return
    raise HarnessError(f"case {case_id}: unknown binding value type: {value_type}")


def _validate_operations(value: Any, case_id: str) -> None:
    _require_type(value, list, f"case {case_id}: operations")
    if len(value) > CORPUS_LIMITS["operations"]:
        raise HarnessError(f"case {case_id}: operation count exceeds the limit")
    binding_count = 0
    finalize_indices: list[int] = []
    for index, item in enumerate(value):
        operation = _require_keys(
            item,
            {"op"},
            f"case {case_id}: operation {index}",
            {"index", "value"},
        )
        kind = _require_string(
            operation["op"], f"case {case_id}: operation {index} op"
        )
        if kind == "bind":
            _require_keys(
                operation,
                {"op", "index", "value"},
                f"case {case_id}: operation {index}",
            )
            parameter_index = _require_integer(
                operation["index"],
                f"case {case_id}: operation {index} index",
            )
            if parameter_index > SQLITE_BIND_INDEX_MAX:
                raise HarnessError(
                    f"case {case_id}: operation {index} index exceeds "
                    "SQLite C int range"
                )
            _validate_value(operation["value"], case_id)
            binding_count += 1
            if binding_count > CORPUS_LIMITS["binding_operations"]:
                raise HarnessError(
                    f"case {case_id}: binding operation count exceeds the limit"
                )
        elif kind in {"step", "reset", "finalize"}:
            _require_keys(
                operation, {"op"}, f"case {case_id}: operation {index}"
            )
            if kind == "finalize":
                finalize_indices.append(index)
        else:
            raise HarnessError(f"case {case_id}: unknown operation: {kind}")

    if value:
        if finalize_indices != [len(value) - 1]:
            raise HarnessError(
                f"case {case_id}: non-empty transcript must end with one FINALIZE"
            )


def _validate_modern_rejection(value: Any, case_id: str) -> None:
    rejection = _require_keys(
        value, {"phase", "primary_code"}, f"case {case_id}: modern_rejection"
    )
    phase = _require_string(
        rejection["phase"], f"case {case_id}: modern_rejection.phase"
    )
    if phase not in {"open", "prepare", "bind", "step", "reset", "finalize"}:
        raise HarnessError(f"case {case_id}: modern_rejection phase is unknown")
    code = _require_string(
        rejection["primary_code"],
        f"case {case_id}: modern_rejection.primary_code",
    )
    if code not in ERROR_CODES:
        raise HarnessError(f"case {case_id}: modern_rejection code is unknown")


def validate_corpus(
    repository_root: pathlib.Path, corpus_path: pathlib.Path
) -> dict[str, Any]:
    try:
        size = corpus_path.stat().st_size
    except OSError as error:
        raise HarnessError(f"cannot stat compatibility corpus {corpus_path}: {error}") from error
    if size > CORPUS_LIMITS["corpus_bytes"]:
        raise HarnessError("compatibility corpus exceeds the file-size limit")

    corpus = _require_keys(
        load_json_strict(corpus_path),
        {
            "schema_version",
            "oracle_profile",
            "limits",
            "databases",
            "supported_matrix",
            "cases",
        },
        "corpus",
    )
    if _require_integer(corpus["schema_version"], "corpus.schema_version") != 1:
        raise HarnessError("corpus.schema_version must be 1")
    limits = _require_keys(corpus["limits"], set(CORPUS_LIMITS), "corpus.limits")
    for name, expected in CORPUS_LIMITS.items():
        if _require_integer(limits[name], f"corpus.limits.{name}") != expected:
            raise HarnessError(f"corpus.limits.{name} must be {expected}")

    profile_path = _repository_path(
        repository_root, corpus["oracle_profile"], "corpus.oracle_profile"
    )
    validate_profile(repository_root, profile_path)

    databases = corpus["databases"]
    _require_type(databases, list, "corpus.databases")
    if not databases:
        raise HarnessError("corpus.databases must not be empty")
    database_paths: dict[str, pathlib.Path] = {}
    for index, item in enumerate(databases):
        database = _require_keys(
            item, {"id", "path", "sha256"}, f"corpus.databases[{index}]"
        )
        database_id = _require_string(
            database["id"], f"corpus.databases[{index}].id"
        )
        if _IDENTIFIER.fullmatch(database_id) is None:
            raise HarnessError(f"invalid database id: {database_id}")
        if database_id in database_paths:
            raise HarnessError(f"duplicate database id: {database_id}")
        path = _repository_path(
            repository_root, database["path"], f"database {database_id} path"
        )
        expected_hash = _require_sha256(
            database["sha256"], f"database {database_id} sha256"
        )
        if _sha256(path) != expected_hash:
            raise HarnessError(f"database hash mismatch: {database_id}")
        database_paths[database_id] = path

    supported_matrix = _require_string_list(
        corpus["supported_matrix"], "corpus.supported_matrix"
    )
    if not supported_matrix:
        raise HarnessError("corpus.supported_matrix must not be empty")
    supported_features = set(supported_matrix)

    cases = corpus["cases"]
    _require_type(cases, list, "corpus.cases")
    if not cases:
        raise HarnessError("corpus.cases must not be empty")
    if len(cases) > CORPUS_LIMITS["cases"]:
        raise HarnessError("corpus case count exceeds the limit")
    case_ids: list[str] = []
    seen_case_ids: set[str] = set()
    for index, item in enumerate(cases):
        case = _require_keys(
            item,
            {
                "id",
                "database",
                "feature",
                "expectation",
                "sql_hex",
                "operations",
            },
            f"corpus.cases[{index}]",
            {"modern_rejection"},
        )
        case_id = _require_string(case["id"], f"corpus.cases[{index}].id")
        if _IDENTIFIER.fullmatch(case_id) is None:
            raise HarnessError(f"invalid case id: {case_id}")
        if case_id in seen_case_ids:
            raise HarnessError(f"duplicate case id: {case_id}")
        seen_case_ids.add(case_id)
        case_ids.append(case_id)

        database_id = _require_string(
            case["database"], f"case {case_id}: database"
        )
        if database_id not in database_paths:
            raise HarnessError(f"case {case_id}: unknown database {database_id}")
        feature = _require_string(case["feature"], f"case {case_id}: feature")
        expectation = _require_string(
            case["expectation"], f"case {case_id}: expectation"
        )
        if expectation not in {"compare", "unsupported"}:
            raise HarnessError(f"case {case_id}: expectation is unknown")
        if expectation == "compare" and feature not in supported_features:
            raise HarnessError(
                f"case {case_id}: compare feature is absent from supported_matrix"
            )

        sql_hex = _require_string(
            case["sql_hex"], f"case {case_id}: sql_hex", nonempty=False
        )
        if _LOWER_HEX.fullmatch(sql_hex) is None:
            raise HarnessError(
                f"case {case_id}: SQL bytes must be lowercase hexadecimal"
            )
        if len(sql_hex) // 2 > CORPUS_LIMITS["sql_bytes"]:
            raise HarnessError(f"case {case_id}: SQL exceeds the byte limit")
        _validate_operations(case["operations"], case_id)
        if (
            sql_hex
            and not case["operations"]
            and (
                expectation == "unsupported"
                or feature == "prepare-errors"
            )
        ):
            raise HarnessError(
                f"case {case_id}: rejected statement requires "
                "a FINALIZE fallback"
            )

        if expectation == "unsupported":
            if "modern_rejection" not in case:
                raise HarnessError(
                    f"case {case_id}: unsupported case requires modern_rejection"
                )
            _validate_modern_rejection(case["modern_rejection"], case_id)
        elif "modern_rejection" in case:
            raise HarnessError(
                f"case {case_id}: compare case must not define modern_rejection"
            )

    normalized = dict(corpus)
    normalized["oracle_profile_path"] = profile_path
    normalized["database_paths"] = database_paths
    normalized["case_ids"] = case_ids
    return normalized


def _validate_output_value(value: Any, label: str) -> None:
    typed = _require_keys(
        value, {"type"}, label, {"value", "bits", "hex"}
    )
    value_type = _require_string(typed["type"], f"{label}.type")
    if value_type == "null":
        _require_keys(typed, {"type"}, label)
        return
    if value_type == "integer":
        _require_keys(typed, {"type", "value"}, label)
        integer = _require_string(typed["value"], f"{label}.value")
        if _INTEGER.fullmatch(integer) is None:
            raise HarnessError(f"{label}.value is not canonical signed decimal")
        parsed = int(integer)
        if parsed < -(1 << 63) or parsed > (1 << 63) - 1:
            raise HarnessError(f"{label}.value is out of range")
        return
    if value_type == "real":
        _require_keys(typed, {"type", "bits"}, label)
        bits = _require_string(typed["bits"], f"{label}.bits")
        if _REAL_BITS.fullmatch(bits) is None:
            raise HarnessError(f"{label}.bits is not a binary64 bit pattern")
        return
    if value_type in {"text", "blob"}:
        _require_keys(typed, {"type", "hex"}, label)
        encoded = _require_string(typed["hex"], f"{label}.hex", nonempty=False)
        if _LOWER_HEX.fullmatch(encoded) is None:
            raise HarnessError(f"{label}.hex is not lowercase hexadecimal")
        if len(encoded) // 2 > CORPUS_LIMITS["value_bytes"]:
            raise HarnessError(f"{label} exceeds the value byte limit")
        return
    raise HarnessError(f"{label}.type is unknown")


def _validate_status(value: Any, label: str) -> None:
    status = _require_keys(
        value,
        {"primary_code", "return_code", "extended_code"},
        label,
        {"message_hex"},
    )
    primary = _require_string(status["primary_code"], f"{label}.primary_code")
    if primary != "ok" and primary not in ERROR_CODES:
        raise HarnessError(f"{label}.primary_code is unknown")
    _require_integer(status["return_code"], f"{label}.return_code")
    _require_integer(status["extended_code"], f"{label}.extended_code")
    if "message_hex" in status:
        message = _require_string(
            status["message_hex"], f"{label}.message_hex", nonempty=False
        )
        if _LOWER_HEX.fullmatch(message) is None:
            raise HarnessError(f"{label}.message_hex is not lowercase hexadecimal")


def _validate_columns(value: Any, label: str) -> None:
    _require_type(value, list, label)
    if len(value) > CORPUS_LIMITS["result_columns"]:
        raise HarnessError(f"{label} exceeds the column limit")
    for index, item in enumerate(value):
        column = _require_keys(
            item, {"name_hex", "declared_type_hex"}, f"{label}[{index}]"
        )
        name = _require_string(
            column["name_hex"], f"{label}[{index}].name_hex", nonempty=False
        )
        if _LOWER_HEX.fullmatch(name) is None:
            raise HarnessError(f"{label}[{index}].name_hex is invalid")
        declared = column["declared_type_hex"]
        if declared is not None:
            declared = _require_string(
                declared,
                f"{label}[{index}].declared_type_hex",
                nonempty=False,
            )
            if _LOWER_HEX.fullmatch(declared) is None:
                raise HarnessError(
                    f"{label}[{index}].declared_type_hex is invalid"
                )


def validate_outcome(value: Any, label: str) -> dict[str, Any]:
    outcome = _require_keys(
        value, {"format_version", "kind"}, label, {
            "status",
            "next_offset",
            "preparation",
            "observations",
            "resource",
            "completed_observations",
        }
    )
    if _require_integer(outcome["format_version"], f"{label}.format_version") != 1:
        raise HarnessError(f"{label}.format_version must be 1")
    kind = _require_string(outcome["kind"], f"{label}.kind")
    if kind in {"open_error", "prepare_error"}:
        _require_keys(outcome, {"format_version", "kind", "status"}, label)
        _validate_status(outcome["status"], f"{label}.status")
        return outcome
    if kind == "empty":
        _require_keys(outcome, {"format_version", "kind", "next_offset"}, label)
        _require_integer(outcome["next_offset"], f"{label}.next_offset")
        return outcome
    if kind == "limit":
        _require_keys(
            outcome,
            {
                "format_version",
                "kind",
                "resource",
                "completed_observations",
            },
            label,
        )
        _require_string(outcome["resource"], f"{label}.resource")
        _require_type(
            outcome["completed_observations"],
            list,
            f"{label}.completed_observations",
        )
        return outcome
    if kind != "statement":
        raise HarnessError(f"{label}.kind is unknown")

    _require_keys(
        outcome,
        {"format_version", "kind", "preparation", "observations"},
        label,
    )
    preparation = _require_keys(
        outcome["preparation"],
        {"next_offset", "parameter_count", "parameter_names", "columns"},
        f"{label}.preparation",
    )
    _require_integer(
        preparation["next_offset"], f"{label}.preparation.next_offset"
    )
    parameter_count = _require_integer(
        preparation["parameter_count"], f"{label}.preparation.parameter_count"
    )
    parameter_names = preparation["parameter_names"]
    _require_type(
        parameter_names, list, f"{label}.preparation.parameter_names"
    )
    if len(parameter_names) != parameter_count:
        raise HarnessError(f"{label}.preparation parameter metadata count mismatch")
    for index, name in enumerate(parameter_names):
        if name is None:
            continue
        name = _require_string(
            name,
            f"{label}.preparation.parameter_names[{index}]",
            nonempty=False,
        )
        if _LOWER_HEX.fullmatch(name) is None:
            raise HarnessError(
                f"{label}.preparation.parameter_names[{index}] is invalid"
            )
    _validate_columns(preparation["columns"], f"{label}.preparation.columns")

    observations = outcome["observations"]
    _require_type(observations, list, f"{label}.observations")
    if len(observations) > CORPUS_LIMITS["operations"]:
        raise HarnessError(f"{label}.observations exceeds the operation limit")
    row_count = 0
    for index, item in enumerate(observations):
        observation = _require_keys(
            item,
            {"operation_index", "op"},
            f"{label}.observations[{index}]",
            {
                "index",
                "status",
                "result",
                "return_code",
                "columns",
                "row",
            },
        )
        if (
            _require_integer(
                observation["operation_index"],
                f"{label}.observations[{index}].operation_index",
            )
            != index
        ):
            raise HarnessError(f"{label}.observations operation index mismatch")
        operation = _require_string(
            observation["op"], f"{label}.observations[{index}].op"
        )
        if operation == "bind":
            _require_keys(
                observation,
                {"operation_index", "op", "index", "status"},
                f"{label}.observations[{index}]",
            )
            _require_integer(
                observation["index"], f"{label}.observations[{index}].index"
            )
            _validate_status(
                observation["status"], f"{label}.observations[{index}].status"
            )
        elif operation in {"reset", "finalize"}:
            _require_keys(
                observation,
                {"operation_index", "op", "status"},
                f"{label}.observations[{index}]",
            )
            _validate_status(
                observation["status"], f"{label}.observations[{index}].status"
            )
        elif operation == "step":
            result = _require_string(
                observation.get("result"),
                f"{label}.observations[{index}].result",
            )
            if result == "row":
                _require_keys(
                    observation,
                    {
                        "operation_index",
                        "op",
                        "result",
                        "return_code",
                        "columns",
                        "row",
                    },
                    f"{label}.observations[{index}]",
                )
                if (
                    _require_integer(
                        observation["return_code"],
                        f"{label}.observations[{index}].return_code",
                    )
                    != _SQLITE_ROW
                ):
                    raise HarnessError(
                        f"{label}.observations[{index}] has wrong ROW code"
                    )
                _validate_columns(
                    observation["columns"],
                    f"{label}.observations[{index}].columns",
                )
                row = observation["row"]
                _require_type(row, list, f"{label}.observations[{index}].row")
                if len(row) != len(observation["columns"]):
                    raise HarnessError(
                        f"{label}.observations[{index}] row width mismatch"
                    )
                for value_index, cell in enumerate(row):
                    _validate_output_value(
                        cell,
                        f"{label}.observations[{index}].row[{value_index}]",
                    )
                row_count += 1
                if row_count > CORPUS_LIMITS["result_rows"]:
                    raise HarnessError(f"{label} exceeds the row limit")
            elif result == "done":
                _require_keys(
                    observation,
                    {"operation_index", "op", "result", "return_code"},
                    f"{label}.observations[{index}]",
                )
                if (
                    _require_integer(
                        observation["return_code"],
                        f"{label}.observations[{index}].return_code",
                    )
                    != _SQLITE_DONE
                ):
                    raise HarnessError(
                        f"{label}.observations[{index}] has wrong DONE code"
                    )
            elif result == "error":
                _require_keys(
                    observation,
                    {"operation_index", "op", "result", "status"},
                    f"{label}.observations[{index}]",
                )
                _validate_status(
                    observation["status"],
                    f"{label}.observations[{index}].status",
                )
            else:
                raise HarnessError(
                    f"{label}.observations[{index}].result is unknown"
                )
        else:
            raise HarnessError(f"{label}.observations[{index}].op is unknown")
    return outcome


def _validate_boundary(value: Any, label: str) -> dict[str, Any]:
    boundary = _require_keys(
        value, {"prepare"}, label, {"first_step", "primary_code"}
    )
    prepare = _require_string(boundary["prepare"], f"{label}.prepare")
    if prepare not in {"statement", "empty", "error", "open_error"}:
        raise HarnessError(f"{label}.prepare is unknown")
    if prepare == "statement":
        first_step = _require_string(
            boundary.get("first_step"), f"{label}.first_step"
        )
        if first_step not in {"row", "done", "error"}:
            raise HarnessError(f"{label}.first_step is unknown")
        if first_step == "error":
            code = _require_string(
                boundary.get("primary_code"), f"{label}.primary_code"
            )
            if code not in ERROR_CODES:
                raise HarnessError(f"{label}.primary_code is unknown")
        elif "primary_code" in boundary:
            raise HarnessError(f"{label}.primary_code is unexpected")
    elif "first_step" in boundary:
        raise HarnessError(f"{label}.first_step is unexpected")
    return boundary


def validate_oracle(
    repository_root: pathlib.Path,
    oracle_path: pathlib.Path,
    corpus_path: pathlib.Path,
    corpus: dict[str, Any],
) -> dict[str, Any]:
    try:
        size = oracle_path.stat().st_size
    except OSError as error:
        raise HarnessError(f"cannot stat compatibility oracle {oracle_path}: {error}") from error
    if size > CORPUS_LIMITS["oracle_bytes"]:
        raise HarnessError("compatibility oracle exceeds the file-size limit")
    oracle = _require_keys(
        load_json_strict(oracle_path),
        {
            "schema_version",
            "oracle_profile_path",
            "oracle_profile_schema_version",
            "oracle_profile_sha256",
            "corpus_path",
            "corpus_sha256",
            "generator",
            "generator_format_version",
            "sqlite",
            "databases",
            "fixture_sources",
            "cases",
        },
        "oracle",
    )
    if _require_integer(oracle["schema_version"], "oracle.schema_version") != 1:
        raise HarnessError("oracle.schema_version must be 1")
    if (
        _require_integer(
            oracle["oracle_profile_schema_version"],
            "oracle.oracle_profile_schema_version",
        )
        != 1
    ):
        raise HarnessError("oracle.oracle_profile_schema_version must be 1")
    if (
        _require_integer(
            oracle["generator_format_version"],
            "oracle.generator_format_version",
        )
        != 1
    ):
        raise HarnessError("oracle.generator_format_version must be 1")

    profile_path = _repository_path(
        repository_root,
        oracle["oracle_profile_path"],
        "oracle.oracle_profile_path",
    )
    if profile_path != corpus["oracle_profile_path"]:
        raise HarnessError("oracle profile path does not match corpus")
    if _sha256(profile_path) != _require_sha256(
        oracle["oracle_profile_sha256"], "oracle.oracle_profile_sha256"
    ):
        raise HarnessError("oracle profile hash mismatch")
    profile = validate_profile(repository_root, profile_path)
    if oracle["oracle_profile_schema_version"] != profile["schema_version"]:
        raise HarnessError("oracle profile schema version mismatch")

    recorded_corpus = _repository_path(
        repository_root, oracle["corpus_path"], "oracle.corpus_path"
    )
    if recorded_corpus != corpus_path.resolve():
        raise HarnessError("oracle corpus path does not match requested corpus")
    if _sha256(corpus_path) != _require_sha256(
        oracle["corpus_sha256"], "oracle.corpus_sha256"
    ):
        raise HarnessError("oracle corpus hash mismatch")
    if oracle["generator"] != profile["generator"]:
        raise HarnessError("oracle generator provenance mismatch")

    sqlite = _require_keys(
        oracle["sqlite"],
        {
            "version",
            "source_id",
            "sqlite3_c_sha256",
            "sqlite3_h_sha256",
            "compile_options",
        },
        "oracle.sqlite",
    )
    if sqlite["version"] != profile["sqlite"]["version"]:
        raise HarnessError("oracle SQLite version mismatch")
    if sqlite["source_id"] != profile["sqlite"]["source_id"]:
        raise HarnessError("oracle SQLite source ID mismatch")
    if sqlite["sqlite3_c_sha256"] != profile["sqlite"]["sqlite3_c_sha256"]:
        raise HarnessError("oracle sqlite3.c hash mismatch")
    if sqlite["sqlite3_h_sha256"] != profile["sqlite"]["sqlite3_h_sha256"]:
        raise HarnessError("oracle sqlite3.h hash mismatch")
    compile_options = _require_string_list(
        sqlite["compile_options"], "oracle.sqlite.compile_options"
    )
    if compile_options != profile["build"]["semantic_compile_options"]:
        raise HarnessError(
            "oracle compile options do not match the pinned semantic profile"
        )

    if oracle["databases"] != corpus["databases"]:
        raise HarnessError("oracle database provenance mismatch")
    if oracle["fixture_sources"] != profile["fixture_sources"]:
        raise HarnessError("oracle fixture-source provenance mismatch")

    oracle_cases = oracle["cases"]
    _require_type(oracle_cases, list, "oracle.cases")
    if len(oracle_cases) != len(corpus["cases"]):
        raise HarnessError("oracle case count mismatch")
    cases_by_id: dict[str, dict[str, Any]] = {}
    for index, (record, case) in enumerate(zip(oracle_cases, corpus["cases"])):
        expectation = case["expectation"]
        required = {"id", "expectation", "outcome" if expectation == "compare" else "boundary"}
        entry = _require_keys(record, required, f"oracle.cases[{index}]")
        if entry["id"] != case["id"] or entry["expectation"] != expectation:
            raise HarnessError(f"oracle case ordering mismatch at {case['id']}")
        if expectation == "compare":
            validate_outcome(entry["outcome"], f"oracle case {case['id']} outcome")
        else:
            _validate_boundary(entry["boundary"], f"oracle case {case['id']} boundary")
        cases_by_id[case["id"]] = entry
    normalized = dict(oracle)
    normalized["cases_by_id"] = cases_by_id
    return normalized


def _protocol_for_case(case: dict[str, Any]) -> bytes:
    lines = ["MSRT1", f"SQL {case['sql_hex']}"]
    for operation in case["operations"]:
        kind = operation["op"]
        if kind == "bind":
            value = operation["value"]
            value_type = value["type"]
            if value_type == "null":
                lines.append(f"BIND {operation['index']} null")
            else:
                payload_key = {
                    "integer": "value",
                    "real": "bits",
                    "text": "hex",
                    "blob": "hex",
                }[value_type]
                lines.append(
                    f"BIND {operation['index']} {value_type} {value[payload_key]}"
                )
        else:
            lines.append(kind.upper())
    return ("\n".join(lines) + "\n").encode("ascii")


def _run_bounded(
    runner: pathlib.Path,
    database: pathlib.Path,
    protocol: bytes,
    limits: dict[str, int],
) -> dict[str, Any]:
    with tempfile.TemporaryFile() as input_file:
        input_file.write(protocol)
        input_file.seek(0)
        try:
            process = subprocess.Popen(
                [str(runner), str(database)],
                stdin=input_file,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as error:
            raise HarnessError(f"cannot launch compatibility runner: {error}") from error

        assert process.stdout is not None
        assert process.stderr is not None
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ, "stdout")
        selector.register(process.stderr, selectors.EVENT_READ, "stderr")
        stdout = bytearray()
        stderr = bytearray()
        deadline = time.monotonic() + limits["child_seconds"]
        try:
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    process.kill()
                    process.wait()
                    raise HarnessError("compatibility runner timed out")
                events = selector.select(remaining)
                if not events:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                        raise HarnessError("compatibility runner timed out")
                    continue
                for key, _ in events:
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    target = stdout if key.data == "stdout" else stderr
                    limit = (
                        limits["output_bytes"] + 1
                        if key.data == "stdout"
                        else limits["stderr_bytes"]
                    )
                    if len(target) + len(chunk) > limit:
                        process.kill()
                        process.wait()
                        raise HarnessError(
                            f"runner {key.data} exceeded the capture limit"
                        )
                    target.extend(chunk)
            remaining = max(0.0, deadline - time.monotonic())
            try:
                return_code = process.wait(timeout=remaining)
            except subprocess.TimeoutExpired as error:
                process.kill()
                process.wait()
                raise HarnessError("compatibility runner timed out") from error
        finally:
            selector.close()

    stderr_text = stderr.decode("utf-8", errors="replace")
    if return_code < 0:
        raise HarnessError(
            f"runner terminated by signal {-return_code}: {stderr_text}"
        )
    if return_code != 0:
        raise HarnessError(f"runner exited with code {return_code}: {stderr_text}")
    if stderr:
        raise HarnessError(f"runner wrote to stderr: {stderr_text}")
    outcome = _load_json_bytes_strict(bytes(stdout), "compatibility runner")
    return validate_outcome(outcome, "runner outcome")


_DIAGNOSTIC_FIELDS = {"return_code", "extended_code", "message_hex"}


def _normalize_contract(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: _normalize_contract(item)
            for key, item in value.items()
            if key not in _DIAGNOSTIC_FIELDS
        }
    if isinstance(value, list):
        return [_normalize_contract(item) for item in value]
    return value


def _first_difference(expected: Any, actual: Any, path: str = "$") -> str:
    if type(expected) is not type(actual):
        return path
    if isinstance(expected, dict):
        expected_keys = set(expected)
        actual_keys = set(actual)
        if expected_keys != actual_keys:
            missing = sorted(expected_keys - actual_keys)
            extra = sorted(actual_keys - expected_keys)
            if missing:
                return f"{path}.{missing[0]}"
            return f"{path}.{extra[0]}"
        for key in expected:
            difference = _first_difference(
                expected[key], actual[key], f"{path}.{key}"
            )
            if difference:
                return difference
        return ""
    if isinstance(expected, list):
        if len(expected) != len(actual):
            return f"{path}.length"
        for index, (expected_item, actual_item) in enumerate(zip(expected, actual)):
            difference = _first_difference(
                expected_item, actual_item, f"{path}[{index}]"
            )
            if difference:
                return difference
        return ""
    return "" if expected == actual else path


def _matches_rejection(actual: dict[str, Any], rejection: dict[str, str]) -> bool:
    phase = rejection["phase"]
    code = rejection["primary_code"]
    if phase == "open":
        return (
            actual.get("kind") == "open_error"
            and actual["status"]["primary_code"] == code
        )
    if phase == "prepare":
        return (
            actual.get("kind") == "prepare_error"
            and actual["status"]["primary_code"] == code
        )
    if actual.get("kind") != "statement":
        return False
    for observation in actual["observations"]:
        if observation["op"] != phase:
            continue
        if phase == "step":
            return (
                observation.get("result") == "error"
                and observation["status"]["primary_code"] == code
            )
        return observation["status"]["primary_code"] == code
    return False


def verify_compatibility(
    repository_root: pathlib.Path,
    corpus_path: pathlib.Path,
    oracle_path: pathlib.Path,
    runner: pathlib.Path,
    case_id: str | None,
) -> tuple[int, int]:
    corpus = validate_corpus(repository_root, corpus_path)
    oracle = validate_oracle(
        repository_root, oracle_path, corpus_path, corpus
    )
    if case_id is None:
        selected = corpus["cases"]
    else:
        selected = [case for case in corpus["cases"] if case["id"] == case_id]
        if not selected:
            raise HarnessError(f"unknown compatibility case: {case_id}")

    compare_count = 0
    unsupported_count = 0
    for case in selected:
        protocol = _protocol_for_case(case)
        database = corpus["database_paths"][case["database"]]
        actual = _run_bounded(runner, database, protocol, corpus["limits"])
        record = oracle["cases_by_id"][case["id"]]
        if case["expectation"] == "compare":
            compare_count += 1
            expected = record["outcome"]
            normalized_expected = _normalize_contract(expected)
            normalized_actual = _normalize_contract(actual)
            if normalized_expected != normalized_actual:
                raise VerificationMismatch(
                    "compatibility mismatch",
                    case,
                    expected,
                    actual,
                    _first_difference(normalized_expected, normalized_actual),
                    protocol,
                    runner,
                    database,
                )
        else:
            unsupported_count += 1
            rejection = case["modern_rejection"]
            if not _matches_rejection(actual, rejection):
                raise VerificationMismatch(
                    "unsupported boundary regression",
                    case,
                    rejection,
                    actual,
                    "$",
                    protocol,
                    runner,
                    database,
                )
    return compare_count, unsupported_count


def _repository_relative(
    repository_root: pathlib.Path, path: pathlib.Path, label: str
) -> str:
    root = repository_root.resolve()
    resolved = path.resolve()
    try:
        return resolved.relative_to(root).as_posix()
    except ValueError as error:
        raise HarnessError(f"{label} must be inside the repository root") from error


def _paths_alias(left: pathlib.Path, right: pathlib.Path) -> bool:
    if left == right:
        return True
    try:
        return left.samefile(right)
    except FileNotFoundError:
        return False
    except OSError as error:
        raise HarnessError(
            f"cannot compare provenance paths {left} and {right}: {error}"
        ) from error


def _write_json_atomic(path: pathlib.Path, value: Any) -> None:
    encoded = (
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    ).encode("utf-8")
    if len(encoded) > CORPUS_LIMITS["oracle_bytes"]:
        raise HarnessError("generated compatibility oracle exceeds the file-size limit")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
        )
    except OSError as error:
        raise HarnessError(f"cannot create compatibility oracle: {error}") from error

    temporary_path = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(encoded)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, path)
    except OSError as error:
        try:
            temporary_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise HarnessError(f"cannot write compatibility oracle: {error}") from error


def regenerate_oracle(
    repository_root: pathlib.Path,
    corpus_path: pathlib.Path,
    output_path: pathlib.Path,
    sqlite_library_path: pathlib.Path,
    sqlite_c_path: pathlib.Path,
    sqlite_h_path: pathlib.Path,
) -> tuple[int, int]:
    root = repository_root.resolve()
    corpus_path = corpus_path.resolve()
    output_path = output_path.resolve()
    sqlite_library_path = sqlite_library_path.resolve()
    sqlite_c_path = sqlite_c_path.resolve()
    sqlite_h_path = sqlite_h_path.resolve()
    _repository_relative(root, output_path, "oracle output")

    corpus = validate_corpus(root, corpus_path)
    profile_path = corpus["oracle_profile_path"]
    profile = validate_profile(root, profile_path)
    protected_paths = {
        corpus_path,
        profile_path,
        profile["generator_path"],
        *profile["fixture_source_paths"],
        *corpus["database_paths"].values(),
        sqlite_library_path,
        sqlite_c_path,
        sqlite_h_path,
    }
    if any(
        _paths_alias(output_path, protected_path)
        for protected_path in protected_paths
    ):
        raise HarnessError("oracle output must not overwrite a provenance input")
    oracle_runner = PinnedSqliteOracle(
        sqlite_library_path,
        profile,
        sqlite_c_path,
        sqlite_h_path,
    )

    records: list[dict[str, Any]] = []
    compare_count = 0
    unsupported_count = 0
    for case in corpus["cases"]:
        database = corpus["database_paths"][case["database"]]
        sql = bytes.fromhex(case["sql_hex"])
        if case["expectation"] == "compare":
            outcome = oracle_runner.run_case(database, sql, case["operations"])
            validate_outcome(outcome, f"generated oracle case {case['id']} outcome")
            records.append(
                {
                    "id": case["id"],
                    "expectation": "compare",
                    "outcome": outcome,
                }
            )
            compare_count += 1
        else:
            boundary = oracle_runner.run_boundary(database, sql)
            _validate_boundary(
                boundary, f"generated oracle case {case['id']} boundary"
            )
            records.append(
                {
                    "id": case["id"],
                    "expectation": "unsupported",
                    "boundary": boundary,
                }
            )
            unsupported_count += 1

    oracle = {
        "schema_version": 1,
        "oracle_profile_path": _repository_relative(
            root, profile_path, "oracle profile"
        ),
        "oracle_profile_schema_version": profile["schema_version"],
        "oracle_profile_sha256": _sha256(profile_path),
        "corpus_path": _repository_relative(root, corpus_path, "corpus"),
        "corpus_sha256": _sha256(corpus_path),
        "generator": profile["generator"],
        "generator_format_version": 1,
        "sqlite": {
            "version": profile["sqlite"]["version"],
            "source_id": profile["sqlite"]["source_id"],
            "sqlite3_c_sha256": profile["sqlite"]["sqlite3_c_sha256"],
            "sqlite3_h_sha256": profile["sqlite"]["sqlite3_h_sha256"],
            "compile_options": list(oracle_runner.compile_options),
        },
        "databases": corpus["databases"],
        "fixture_sources": profile["fixture_sources"],
        "cases": records,
    }
    _write_json_atomic(output_path, oracle)
    validate_oracle(root, output_path, corpus_path, corpus)
    return compare_count, unsupported_count


class PinnedSqliteOracle:
    def __init__(
        self,
        library_path: pathlib.Path,
        profile: dict[str, Any],
        sqlite_c_path: pathlib.Path,
        sqlite_h_path: pathlib.Path,
    ) -> None:
        try:
            self.library = ctypes.CDLL(str(library_path))
        except OSError as error:
            raise HarnessError(f"cannot load pinned SQLite library: {error}") from error
        self._configure_api()
        self.profile = profile
        self._validate_identity(sqlite_c_path, sqlite_h_path)

    def _configure_api(self) -> None:
        library = self.library
        library.sqlite3_libversion.restype = ctypes.c_char_p
        library.sqlite3_sourceid.restype = ctypes.c_char_p
        library.sqlite3_compileoption_get.argtypes = [ctypes.c_int]
        library.sqlite3_compileoption_get.restype = ctypes.c_char_p
        library.sqlite3_open_v2.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_int,
            ctypes.c_char_p,
        ]
        library.sqlite3_open_v2.restype = ctypes.c_int
        library.sqlite3_close_v2.argtypes = [ctypes.c_void_p]
        library.sqlite3_close_v2.restype = ctypes.c_int
        library.sqlite3_extended_result_codes.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
        ]
        library.sqlite3_extended_result_codes.restype = ctypes.c_int
        library.sqlite3_db_config.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_db_config.restype = ctypes.c_int
        library.sqlite3_limit.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        library.sqlite3_limit.restype = ctypes.c_int
        library.sqlite3_errcode.argtypes = [ctypes.c_void_p]
        library.sqlite3_errcode.restype = ctypes.c_int
        library.sqlite3_extended_errcode.argtypes = [ctypes.c_void_p]
        library.sqlite3_extended_errcode.restype = ctypes.c_int
        library.sqlite3_errmsg.argtypes = [ctypes.c_void_p]
        library.sqlite3_errmsg.restype = ctypes.c_void_p
        library.sqlite3_exec.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        library.sqlite3_exec.restype = ctypes.c_int
        library.sqlite3_free.argtypes = [ctypes.c_void_p]
        library.sqlite3_free.restype = None
        library.sqlite3_prepare_v3.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_uint,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_void_p),
        ]
        library.sqlite3_prepare_v3.restype = ctypes.c_int
        library.sqlite3_bind_parameter_count.argtypes = [ctypes.c_void_p]
        library.sqlite3_bind_parameter_count.restype = ctypes.c_int
        library.sqlite3_bind_parameter_name.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
        ]
        library.sqlite3_bind_parameter_name.restype = ctypes.c_void_p
        library.sqlite3_bind_null.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_bind_null.restype = ctypes.c_int
        library.sqlite3_bind_int64.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int64,
        ]
        library.sqlite3_bind_int64.restype = ctypes.c_int
        library.sqlite3_bind_double.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_double,
        ]
        library.sqlite3_bind_double.restype = ctypes.c_int
        library.sqlite3_bind_text.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
        ]
        library.sqlite3_bind_text.restype = ctypes.c_int
        library.sqlite3_bind_blob.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
        ]
        library.sqlite3_bind_blob.restype = ctypes.c_int
        library.sqlite3_column_count.argtypes = [ctypes.c_void_p]
        library.sqlite3_column_count.restype = ctypes.c_int
        library.sqlite3_column_name.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_column_name.restype = ctypes.c_void_p
        library.sqlite3_column_decltype.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_column_decltype.restype = ctypes.c_void_p
        library.sqlite3_column_type.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_column_type.restype = ctypes.c_int
        library.sqlite3_column_int64.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_column_int64.restype = ctypes.c_int64
        library.sqlite3_column_double.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_column_double.restype = ctypes.c_double
        library.sqlite3_column_text.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_column_text.restype = ctypes.c_void_p
        library.sqlite3_column_blob.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_column_blob.restype = ctypes.c_void_p
        library.sqlite3_column_bytes.argtypes = [ctypes.c_void_p, ctypes.c_int]
        library.sqlite3_column_bytes.restype = ctypes.c_int
        library.sqlite3_step.argtypes = [ctypes.c_void_p]
        library.sqlite3_step.restype = ctypes.c_int
        library.sqlite3_reset.argtypes = [ctypes.c_void_p]
        library.sqlite3_reset.restype = ctypes.c_int
        library.sqlite3_finalize.argtypes = [ctypes.c_void_p]
        library.sqlite3_finalize.restype = ctypes.c_int

    def _validate_identity(
        self, sqlite_c_path: pathlib.Path, sqlite_h_path: pathlib.Path
    ) -> None:
        sqlite = self.profile["sqlite"]
        version = self.library.sqlite3_libversion().decode("ascii")
        if version != sqlite["version"]:
            raise HarnessError(
                f"SQLite version mismatch: expected {sqlite['version']}, got {version}"
            )
        source_id = self.library.sqlite3_sourceid().decode("ascii")
        if source_id != sqlite["source_id"]:
            raise HarnessError("SQLite source ID mismatch")
        if _sha256(sqlite_c_path) != sqlite["sqlite3_c_sha256"]:
            raise HarnessError("sqlite3.c hash mismatch")
        if _sha256(sqlite_h_path) != sqlite["sqlite3_h_sha256"]:
            raise HarnessError("sqlite3.h hash mismatch")

        raw_compile_options: list[str] = []
        index = 0
        while True:
            option = self.library.sqlite3_compileoption_get(index)
            if option is None:
                break
            raw_compile_options.append(option.decode("ascii"))
            index += 1
        semantic_options = sorted(
            option
            for option in raw_compile_options
            if not option.startswith(_DIAGNOSTIC_COMPILE_OPTION_PREFIXES)
        )
        diagnostic_options = sorted(
            option
            for option in raw_compile_options
            if option.startswith(_DIAGNOSTIC_COMPILE_OPTION_PREFIXES)
        )
        if semantic_options != self.profile["build"]["semantic_compile_options"]:
            raise HarnessError(
                "SQLite semantic compile options do not match "
                "the pinned profile"
            )
        self.compile_options = tuple(semantic_options)
        self.diagnostic_compile_options = tuple(diagnostic_options)

    def _open(
        self, path: pathlib.Path, flags: int
    ) -> tuple[ctypes.c_void_p, int]:
        database = ctypes.c_void_p()
        result = self.library.sqlite3_open_v2(
            os.fsencode(path), ctypes.byref(database), flags, None
        )
        return database, result

    def _close(self, database: ctypes.c_void_p) -> None:
        if database.value is None:
            return
        result = self.library.sqlite3_close_v2(database)
        if result != _SQLITE_OK:
            raise HarnessError(f"sqlite3_close_v2 failed with code {result}")

    def _configure_connection(self, database: ctypes.c_void_p) -> None:
        extended = 1 if self.profile["connection"]["extended_result_codes"] else 0
        result = self.library.sqlite3_extended_result_codes(database, extended)
        if result != _SQLITE_OK:
            raise HarnessError(
                f"sqlite3_extended_result_codes failed with code {result}"
            )
        for name, expected in self.profile["connection"]["db_config"].items():
            effective = ctypes.c_int()
            result = self.library.sqlite3_db_config(
                database,
                _DB_CONFIG_IDS[name],
                ctypes.c_int(expected),
                ctypes.byref(effective),
            )
            if result != _SQLITE_OK or effective.value != expected:
                raise HarnessError(
                    f"SQLite db_config {name} did not accept value {expected}"
                )
        for name, expected in self.profile["connection"]["limits"].items():
            self.library.sqlite3_limit(database, _LIMIT_IDS[name], expected)
            effective = self.library.sqlite3_limit(database, _LIMIT_IDS[name], -1)
            if effective != expected:
                raise HarnessError(
                    f"SQLite limit {name} did not accept value {expected}"
                )

    def _message_bytes(self, database: ctypes.c_void_p) -> bytes:
        pointer = self.library.sqlite3_errmsg(database)
        return b"" if pointer is None else ctypes.string_at(pointer)

    def _status(self, database: ctypes.c_void_p, result: int) -> dict[str, Any]:
        primary = result & 0xFF
        if primary not in _PRIMARY_CODE_NAMES:
            raise HarnessError(f"unknown SQLite primary result code: {primary}")
        status: dict[str, Any] = {
            "primary_code": _PRIMARY_CODE_NAMES[primary],
            "return_code": result,
            "extended_code": (
                0
                if result == _SQLITE_OK
                else self.library.sqlite3_extended_errcode(database)
            ),
        }
        if result != _SQLITE_OK:
            message = self._message_bytes(database)
            if message:
                status["message_hex"] = message.hex()
        return status

    def create_database(self, path: pathlib.Path, sql: str) -> None:
        if path.exists():
            path.unlink()
        database, result = self._open(
            path, _SQLITE_OPEN_READWRITE | _SQLITE_OPEN_CREATE
        )
        if result != _SQLITE_OK:
            status = self._status(database, result)
            self._close(database)
            raise HarnessError(f"cannot create SQLite fixture: {status}")
        try:
            self._configure_connection(database)
            error_pointer = ctypes.c_void_p()
            result = self.library.sqlite3_exec(
                database,
                sql.encode("utf-8"),
                None,
                None,
                ctypes.byref(error_pointer),
            )
            if result != _SQLITE_OK:
                if error_pointer.value is not None:
                    message = ctypes.string_at(error_pointer.value).decode(
                        "utf-8", errors="replace"
                    )
                    self.library.sqlite3_free(error_pointer)
                else:
                    message = self._message_bytes(database).decode(
                        "utf-8", errors="replace"
                    )
                raise HarnessError(
                    f"cannot initialize SQLite fixture: {result}: {message}"
                )
        finally:
            self._close(database)

    def _prepare(
        self, database: ctypes.c_void_p, sql: bytes
    ) -> tuple[ctypes.c_void_p, int, int]:
        buffer = ctypes.create_string_buffer(len(sql) + 1)
        if sql:
            ctypes.memmove(buffer, sql, len(sql))
        statement = ctypes.c_void_p()
        tail = ctypes.c_void_p()
        result = self.library.sqlite3_prepare_v3(
            database,
            ctypes.c_void_p(ctypes.addressof(buffer)),
            len(sql),
            0,
            ctypes.byref(statement),
            ctypes.byref(tail),
        )
        tail_address = tail.value
        if tail_address is None:
            raise HarnessError("sqlite3_prepare_v3 returned a null tail pointer")
        begin = ctypes.addressof(buffer)
        if tail_address < begin or tail_address > begin + len(sql):
            raise HarnessError("sqlite3_prepare_v3 returned an out-of-range tail")
        return statement, result, tail_address - begin

    def _columns(self, statement: ctypes.c_void_p) -> list[dict[str, Any]]:
        count = self.library.sqlite3_column_count(statement)
        columns: list[dict[str, Any]] = []
        for index in range(count):
            name_pointer = self.library.sqlite3_column_name(statement, index)
            if name_pointer is None:
                raise HarnessError("sqlite3_column_name returned null")
            declared_pointer = self.library.sqlite3_column_decltype(statement, index)
            columns.append(
                {
                    "name_hex": ctypes.string_at(name_pointer).hex(),
                    "declared_type_hex": (
                        None
                        if declared_pointer is None
                        else ctypes.string_at(declared_pointer).hex()
                    ),
                }
            )
        return columns

    def _parameter_names(
        self, statement: ctypes.c_void_p
    ) -> tuple[int, list[str | None]]:
        count = self.library.sqlite3_bind_parameter_count(statement)
        names: list[str | None] = []
        for index in range(1, count + 1):
            pointer = self.library.sqlite3_bind_parameter_name(statement, index)
            names.append(None if pointer is None else ctypes.string_at(pointer).hex())
        return count, names

    def _column_value(
        self,
        database: ctypes.c_void_p,
        statement: ctypes.c_void_p,
        index: int,
    ) -> dict[str, str]:
        value_type = self.library.sqlite3_column_type(statement, index)
        if value_type == _SQLITE_NULL:
            return {"type": "null"}
        if value_type == _SQLITE_INTEGER:
            return {
                "type": "integer",
                "value": str(self.library.sqlite3_column_int64(statement, index)),
            }
        if value_type == _SQLITE_FLOAT:
            value = self.library.sqlite3_column_double(statement, index)
            return {"type": "real", "bits": struct.pack(">d", value).hex()}
        if value_type == _SQLITE_TEXT:
            pointer = self.library.sqlite3_column_text(statement, index)
            length = self.library.sqlite3_column_bytes(statement, index)
            if self.library.sqlite3_errcode(database) == _SQLITE_NOMEM:
                raise HarnessError("SQLite ran out of memory extracting TEXT")
            if pointer is None and length != 0:
                raise HarnessError("sqlite3_column_text returned null for nonempty TEXT")
            data = b"" if length == 0 else ctypes.string_at(pointer, length)
            return {"type": "text", "hex": data.hex()}
        if value_type == _SQLITE_BLOB:
            pointer = self.library.sqlite3_column_blob(statement, index)
            length = self.library.sqlite3_column_bytes(statement, index)
            if self.library.sqlite3_errcode(database) == _SQLITE_NOMEM:
                raise HarnessError("SQLite ran out of memory extracting BLOB")
            if pointer is None and length != 0:
                raise HarnessError("sqlite3_column_blob returned null for nonempty BLOB")
            data = b"" if length == 0 else ctypes.string_at(pointer, length)
            return {"type": "blob", "hex": data.hex()}
        raise HarnessError(f"unknown SQLite column type: {value_type}")

    def _row(
        self, database: ctypes.c_void_p, statement: ctypes.c_void_p
    ) -> list[dict[str, str]]:
        return [
            self._column_value(database, statement, index)
            for index in range(self.library.sqlite3_column_count(statement))
        ]

    def _bind(
        self,
        database: ctypes.c_void_p,
        statement: ctypes.c_void_p,
        index: int,
        value: dict[str, Any],
    ) -> dict[str, Any]:
        if index < 0 or index > SQLITE_BIND_INDEX_MAX:
            raise HarnessError("bind index exceeds SQLite C int range")
        value_type = value["type"]
        if value_type == "null":
            result = self.library.sqlite3_bind_null(statement, index)
        elif value_type == "integer":
            result = self.library.sqlite3_bind_int64(
                statement, index, int(value["value"])
            )
        elif value_type == "real":
            number = struct.unpack(">d", bytes.fromhex(value["bits"]))[0]
            result = self.library.sqlite3_bind_double(statement, index, number)
        elif value_type in {"text", "blob"}:
            data = bytes.fromhex(value["hex"])
            if value_type == "text":
                data.decode("utf-8")
            buffer = ctypes.create_string_buffer(max(1, len(data)))
            if data:
                ctypes.memmove(buffer, data, len(data))
            function = (
                self.library.sqlite3_bind_text
                if value_type == "text"
                else self.library.sqlite3_bind_blob
            )
            result = function(
                statement,
                index,
                ctypes.c_void_p(ctypes.addressof(buffer)),
                len(data),
                _SQLITE_TRANSIENT,
            )
        else:
            raise HarnessError(f"unknown binding value type: {value_type}")
        return self._status(database, result)

    def run_case(
        self, path: pathlib.Path, sql: bytes, operations: list[dict[str, Any]]
    ) -> dict[str, Any]:
        database, result = self._open(path, _SQLITE_OPEN_READONLY)
        if result != _SQLITE_OK:
            try:
                return {
                    "format_version": 1,
                    "kind": "open_error",
                    "status": self._status(database, result),
                }
            finally:
                self._close(database)

        statement = ctypes.c_void_p()
        try:
            self._configure_connection(database)
            statement, result, next_offset = self._prepare(database, sql)
            if result != _SQLITE_OK:
                return {
                    "format_version": 1,
                    "kind": "prepare_error",
                    "status": self._status(database, result),
                }
            if statement.value is None:
                if operations:
                    raise HarnessError("empty SQLite statement has operations")
                return {
                    "format_version": 1,
                    "kind": "empty",
                    "next_offset": next_offset,
                }
            if not operations or operations[-1] != {"op": "finalize"}:
                raise HarnessError("SQLite statement transcript must end with FINALIZE")

            parameter_count, parameter_names = self._parameter_names(statement)
            outcome: dict[str, Any] = {
                "format_version": 1,
                "kind": "statement",
                "preparation": {
                    "next_offset": next_offset,
                    "parameter_count": parameter_count,
                    "parameter_names": parameter_names,
                    "columns": self._columns(statement),
                },
                "observations": [],
            }
            observations = outcome["observations"]
            for operation_index, operation in enumerate(operations):
                kind = operation["op"]
                observation: dict[str, Any] = {
                    "operation_index": operation_index,
                    "op": kind,
                }
                if kind == "bind":
                    observation["index"] = operation["index"]
                    observation["status"] = self._bind(
                        database,
                        statement,
                        operation["index"],
                        operation["value"],
                    )
                elif kind == "step":
                    result = self.library.sqlite3_step(statement)
                    if result == _SQLITE_ROW:
                        observation.update(
                            {
                                "result": "row",
                                "return_code": _SQLITE_ROW,
                                "columns": self._columns(statement),
                                "row": self._row(database, statement),
                            }
                        )
                    elif result == _SQLITE_DONE:
                        observation.update(
                            {"result": "done", "return_code": _SQLITE_DONE}
                        )
                    else:
                        observation.update(
                            {
                                "result": "error",
                                "status": self._status(database, result),
                            }
                        )
                elif kind == "reset":
                    result = self.library.sqlite3_reset(statement)
                    observation["status"] = self._status(database, result)
                elif kind == "finalize":
                    result = self.library.sqlite3_finalize(statement)
                    statement = ctypes.c_void_p()
                    observation["status"] = self._status(database, result)
                else:
                    raise HarnessError(f"unknown transcript operation: {kind}")
                observations.append(observation)
            return outcome
        finally:
            if statement.value is not None:
                self.library.sqlite3_finalize(statement)
            self._close(database)

    def run_boundary(self, path: pathlib.Path, sql: bytes) -> dict[str, Any]:
        database, result = self._open(path, _SQLITE_OPEN_READONLY)
        if result != _SQLITE_OK:
            try:
                return {
                    "prepare": "open_error",
                    "primary_code": self._status(database, result)["primary_code"],
                }
            finally:
                self._close(database)

        statement = ctypes.c_void_p()
        try:
            self._configure_connection(database)
            statement, result, _ = self._prepare(database, sql)
            if result != _SQLITE_OK:
                return {
                    "prepare": "error",
                    "primary_code": self._status(database, result)["primary_code"],
                }
            if statement.value is None:
                return {"prepare": "empty"}
            step_result = self.library.sqlite3_step(statement)
            if step_result == _SQLITE_ROW:
                first_step = "row"
            elif step_result == _SQLITE_DONE:
                first_step = "done"
            else:
                first_step = "error"
            boundary: dict[str, Any] = {
                "prepare": "statement",
                "first_step": first_step,
            }
            if first_step == "error":
                boundary["primary_code"] = self._status(
                    database, step_result
                )["primary_code"]
            return boundary
        finally:
            if statement.value is not None:
                self.library.sqlite3_finalize(statement)
            self._close(database)


def _plural(count: int, singular: str, plural: str | None = None) -> str:
    return singular if count == 1 else (plural if plural is not None else f"{singular}s")


class _HarnessArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise HarnessError(message)


def _argument_parser() -> argparse.ArgumentParser:
    parser = _HarnessArgumentParser()
    subcommands = parser.add_subparsers(dest="command", required=True)

    verify = subcommands.add_parser("verify")
    verify.add_argument("--repository-root", type=pathlib.Path, required=True)
    verify.add_argument("--corpus", type=pathlib.Path, required=True)
    verify.add_argument("--oracle", type=pathlib.Path, required=True)
    verify.add_argument("--runner", type=pathlib.Path, required=True)
    verify.add_argument("--case")

    regenerate = subcommands.add_parser("regenerate")
    regenerate.add_argument("--repository-root", type=pathlib.Path, required=True)
    regenerate.add_argument("--corpus", type=pathlib.Path, required=True)
    regenerate.add_argument("--output", type=pathlib.Path, required=True)
    regenerate.add_argument("--sqlite-library", type=pathlib.Path, required=True)
    regenerate.add_argument("--sqlite-c", type=pathlib.Path, required=True)
    regenerate.add_argument("--sqlite-h", type=pathlib.Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    try:
        arguments = _argument_parser().parse_args(argv)
        if arguments.command == "verify":
            compare_count, unsupported_count = verify_compatibility(
                arguments.repository_root,
                arguments.corpus,
                arguments.oracle,
                arguments.runner,
                arguments.case,
            )
            print(
                f"verified {compare_count} compatibility "
                f"{_plural(compare_count, 'case')} and {unsupported_count} "
                f"unsupported {_plural(unsupported_count, 'boundary', 'boundaries')}"
            )
            return 0
        compare_count, unsupported_count = regenerate_oracle(
            arguments.repository_root,
            arguments.corpus,
            arguments.output,
            arguments.sqlite_library,
            arguments.sqlite_c,
            arguments.sqlite_h,
        )
        print(
            f"generated {compare_count} compatibility "
            f"{_plural(compare_count, 'case')} and {unsupported_count} "
            f"unsupported {_plural(unsupported_count, 'boundary', 'boundaries')}"
        )
        return 0
    except VerificationMismatch as error:
        sys.stderr.write(error.render())
        return 2
    except HarnessError as error:
        print(f"read compatibility error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
