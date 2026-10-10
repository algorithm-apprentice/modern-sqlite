#!/usr/bin/env python3

import argparse
import ctypes
import dataclasses
import decimal
import fractions
import hashlib
import json
import math
import os
import pathlib
import platform
import re
import selectors
import shlex
import signal
import shutil
import subprocess
import sys
import time
from typing import Any


class HarnessError(RuntimeError):
    pass


class PerformanceMismatch(RuntimeError):
    pass


class ChildExecutionError(HarnessError):
    def __init__(
        self,
        message: str,
        *,
        returncode: int,
        stdout: bytes,
        stderr: bytes,
        elapsed_ns: int,
        timed_out: bool,
    ) -> None:
        super().__init__(message)
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr
        self.elapsed_ns = elapsed_ns
        self.timed_out = timed_out


class BaselineChildFailure(HarnessError):
    def __init__(self, message: str, record: dict[str, Any]) -> None:
        super().__init__(message)
        self.record = record


class BaselineChildMismatch(PerformanceMismatch):
    def __init__(self, message: str, record: dict[str, Any]) -> None:
        super().__init__(message)
        self.record = record


class _ParseFailure(RuntimeError):
    pass


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        self.print_usage(sys.stderr)
        raise _ParseFailure(f"{self.prog}: error: {message}")


SCHEMA_VERSION = 1
WORKLOAD_SEMANTICS_VERSION = 1
COMPLETION_SCHEMA_VERSION = 1
DIAGNOSTIC_SCHEMA_VERSION = 1
MINIMUM_WALL_NS = 200_000_000
MAX_BASELINE_ARTIFACTS = 100
ENFORCE_GUARD_ON_VALIDATION = True
CACHE_PAGES = 512
SQLITE_VERSION = "3.54.0"
SQLITE_SOURCE_ID = (
    "2026-10-02 20:18:07 "
    "65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2"
)
_SQLITE_OPEN_READONLY = 0x00000001
_SQLITE_OK = 0
_SQLITE_ROW = 100
_SQLITE_DONE = 101

EXPECTED_CASE_IDS = (
    "point-present-ipk-fit",
    "point-missing-ipk-fit",
    "scan-ipk-fit",
    "point-present-ipk-pressure",
    "point-missing-ipk-pressure",
    "scan-ipk-pressure",
)

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
    "cache_hits",
    "cache_misses",
    "cache_writes",
    "cache_bytes_current",
    "vm_steps",
    "fullscan_steps",
    "statement_runs",
    "reprepares",
    "malloc_count_current",
    "malloc_count_highwater",
    "malloc_size_highwater",
)

_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_FINGERPRINT = re.compile(r"[0-9a-f]{16}\Z")
_IDENTIFIER = re.compile(r"[a-z][a-z0-9_-]*\Z")

_MANIFEST_KEYS = {
    "schema_version",
    "workload_semantics_version",
    "sqlite_profile",
    "sqlite_semantic_compile_options",
    "configuration",
    "permutation",
    "rounds",
    "fixtures",
    "cases",
    "guard",
}
_CONFIGURATION_KEYS = {
    "page_size",
    "cache_pages",
    "mmap_bytes",
    "temp_store",
    "synchronous",
    "journal_mode",
    "query_only",
    "thread_mode",
}
_PERMUTATION_KEYS = {
    "algorithm",
    "fingerprint",
    "fit_seed",
    "pressure_seed",
}
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
    "content_fingerprint",
    "present_order_fingerprint",
    "missing_order_fingerprint",
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
_EXPECTED_GROUP_KEYS = {"warmup", "measured", "diagnostic", "verification"}
_WORK_KEYS = {
    "operations",
    "items",
    "rows",
    "bytes",
    "result_hits",
    "result_misses",
    "digest",
}
_GUARD_KEYS = {"maximum_wall_ratio", "maximum_cpu_ratio"}
_RATIO_LIMIT_KEYS = {"numerator", "denominator"}

_RAW_TIMING_KEYS = {
    "schema_version",
    "completion_schema_version",
    "workload_semantics_version",
    "mode",
    "run_kind",
    "engine",
    "case",
    "build",
    "sqlite",
    "effective_configuration",
    "timer",
    "warmup",
    "repetitions",
    "completion",
}
_RAW_DIAGNOSTIC_KEYS = {
    "schema_version",
    "completion_schema_version",
    "diagnostic_schema_version",
    "workload_semantics_version",
    "mode",
    "engine",
    "case",
    "build",
    "sqlite",
    "effective_configuration",
    "work",
    "counters",
    "completion",
}
_BUILD_KEYS = {"build_type", "instrumentation", "sanitizers", "coverage"}
_SQLITE_KEYS = {"version", "source_id", "compile_options"}
_TIMER_KEYS = {"wall", "cpu"}
_REPETITION_KEYS = _WORK_KEYS | {"index", "wall_ns", "cpu_ns"}
_COMPLETION_KEYS = {
    "status",
    "session_opens",
    "statement_prepares",
    "statement_finalizes",
    "statement_resets",
    "pre_verifications",
    "post_verifications",
}
_COUNTER_GROUP_KEYS = {"modern", "sqlite"}
_AGGREGATE_KEYS = {
    "schema_version",
    "workload_semantics_version",
    "completion_schema_version",
    "diagnostic_schema_version",
    "status",
    "run_manifest",
    "guard",
    "cases",
    "guard_passed",
}
_AGGREGATE_RUN_MANIFEST_KEYS = {"path", "sha256"}
_AGGREGATE_CASE_KEYS = {"case", "engines", "ratios", "guard_passed"}
_AGGREGATE_ENGINE_KEYS = {
    "repetitions",
    "rounds",
    "wall_median_ns",
    "cpu_median_ns",
}
_AGGREGATE_REPETITION_KEYS = _REPETITION_KEYS | {"round"}
_AGGREGATE_ROUND_KEYS = {"index", "wall_median_ns", "cpu_median_ns"}
_AGGREGATE_RATIOS_KEYS = {"aggregate", "rounds", "bounds"}
_AGGREGATE_METRIC_RATIOS_KEYS = {"wall", "cpu"}
_AGGREGATE_ROUND_RATIO_KEYS = {"index", "wall", "cpu"}
_AGGREGATE_BOUNDS_KEYS = {"wall", "cpu"}
_AGGREGATE_BOUND_KEYS = {"minimum", "maximum"}
_EXACT_RATIO_KEYS = {"numerator", "denominator", "decimal"}
_IDENTITY_KEYS = {"schema_version", "mode", "build", "source", "sqlite"}
_IDENTITY_BUILD_KEYS = {
    "architecture",
    "build_type",
    "compiler",
    "cplusplus",
    "coverage",
    "instrumentation",
    "sanitizers",
    "standard_library",
}
_IDENTITY_NAMED_VERSION_KEYS = {"id", "version"}
_IDENTITY_SOURCE_KEYS = {"revision", "tree"}
_RUN_MANIFEST_KEYS = {
    "schema_version",
    "workload_semantics_version",
    "completion_schema_version",
    "diagnostic_schema_version",
    "status",
    "workload_manifest",
    "source",
    "sqlite",
    "build",
    "host",
    "inputs",
    "binaries",
    "schedule",
    "timing_runs",
    "diagnostic_runs",
}
_ARTIFACT_KEYS = {"path", "sha256", "size_bytes"}
_SOURCE_KEYS = {
    "revision",
    "tree",
    "clean",
    "status_sha256",
    "worktree_content_sha256",
}
_SQLITE_PROVENANCE_KEYS = {
    "version",
    "source_id",
    "compile_options",
    "sqlite3_c_sha256",
    "sqlite3_h_sha256",
    "library_role",
}
_BUILD_PROVENANCE_KEYS = {
    "cmake_version",
    "generator",
    "build_type",
    "c_compiler",
    "c_compiler_version",
    "c_base_flags",
    "c_release_flags",
    "c_effective_flags",
    "cxx_compiler",
    "cxx_compiler_version",
    "cxx_base_flags",
    "cxx_release_flags",
    "cxx_effective_flags",
    "linker_base_flags",
    "linker_release_flags",
    "linker_effective_flags",
    "actual_sqlite_c_compile_flags",
    "actual_modern_cxx_compile_flags",
    "actual_harness_cxx_compile_flags",
    "actual_timing_link_flags",
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
_INPUT_KEYS = {"role", "path", "sha256", "size_bytes"}
_BINARY_GROUP_KEYS = {"timing", "diagnostic"}
_BINARY_KEYS = {"logical_name", "sha256", "size_bytes", "identity"}
_DATABASE_RUN_KEYS = {
    "fixture",
    "source_path",
    "copy_path",
    "before_sha256",
    "after_sha256",
    "sidecars_remaining",
    "cleanup",
}
_PROCESS_KEYS = {"returncode", "timed_out", "runner_elapsed_ns"}
_TIMING_RUN_KEYS = {
    "ordinal",
    "round",
    "case",
    "engine",
    "logical_argv",
    "database",
    "process",
    "stdout",
    "stderr",
}
_DIAGNOSTIC_RUN_KEYS = _TIMING_RUN_KEYS - {"round"}
_UINT64_MASK = (1 << 64) - 1
_FNV1A64_OFFSET = 0xCBF29CE484222325
_FNV1A64_PRIME = 0x100000001B3
_SPLITMIX64_INCREMENT = 0x9E3779B97F4A7C15


@dataclasses.dataclass(frozen=True)
class ChildResult:
    returncode: int
    stdout: bytes
    stderr: bytes
    elapsed_ns: int


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


def load_json_bytes_strict(data: bytes, label: str) -> Any:
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


def _require_exact_keys(
    value: dict[str, Any],
    expected: set[str],
    label: str,
) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise HarnessError(
            f"{label} keys are invalid; missing={missing}, extra={extra}"
        )


def _require_integer(
    value: Any,
    label: str,
    *,
    minimum: int = 0,
) -> int:
    _require_type(value, int, label)
    if value < minimum:
        raise HarnessError(f"{label} must be at least {minimum}")
    return value


def _require_string(value: Any, label: str) -> str:
    _require_type(value, str, label)
    if not value:
        raise HarnessError(f"{label} must be nonempty")
    return value


def _require_identifier(value: Any, label: str) -> str:
    text = _require_string(value, label)
    if _IDENTIFIER.fullmatch(text) is None:
        raise HarnessError(f"{label} must be a lowercase identifier")
    return text


def _require_sha256(value: Any, label: str) -> str:
    text = _require_string(value, label)
    if _SHA256.fullmatch(text) is None:
        raise HarnessError(f"{label} must be a lowercase SHA-256")
    return text


def _require_fingerprint(value: Any, label: str) -> str:
    text = _require_string(value, label)
    if _FINGERPRINT.fullmatch(text) is None:
        raise HarnessError(f"{label} must be a lowercase 64-bit fingerprint")
    return text


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise HarnessError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _resolve_repository_file(
    repository_root: pathlib.Path,
    relative: Any,
    label: str,
) -> pathlib.Path:
    text = _require_string(relative, label)
    relative_path = pathlib.PurePosixPath(text)
    if (
        relative_path.is_absolute()
        or ".." in relative_path.parts
        or "." in relative_path.parts
    ):
        raise HarnessError(f"{label} must be a normalized repository-relative path")
    root = repository_root.resolve()
    path = (root / pathlib.Path(*relative_path.parts)).resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise HarnessError(f"{label} escapes the repository root") from error
    if not path.is_file():
        raise HarnessError(f"{label} is not a file: {path}")
    return path


def paths_refer_to_same_file(left: pathlib.Path, right: pathlib.Path) -> bool:
    try:
        if left.exists() and right.exists() and left.samefile(right):
            return True
    except OSError:
        pass
    left_normalized = os.path.normcase(str(left.resolve(strict=False)))
    right_normalized = os.path.normcase(str(right.resolve(strict=False)))
    return left_normalized == right_normalized


def validate_new_output_path(
    output: pathlib.Path,
    *,
    protected_paths: list[pathlib.Path],
) -> pathlib.Path:
    if os.path.lexists(output):
        raise HarnessError(f"output path already exists: {output}")
    parent = output.parent
    if not parent.is_dir():
        raise HarnessError(f"output parent is not a directory: {parent}")
    resolved = output.resolve(strict=False)
    for protected in protected_paths:
        if paths_refer_to_same_file(resolved, protected):
            raise HarnessError(
                f"output path aliases protected input: {protected}"
            )
    return resolved


def _splitmix64(state: int) -> tuple[int, int]:
    state = (state + _SPLITMIX64_INCREMENT) & _UINT64_MASK
    value = state
    value = (
        ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9)
        & _UINT64_MASK
    )
    value = (
        ((value ^ (value >> 27)) * 0x94D049BB133111EB)
        & _UINT64_MASK
    )
    return state, (value ^ (value >> 31)) & _UINT64_MASK


def _uniform_bounded(state: int, bound: int) -> tuple[int, int]:
    if bound <= 0 or bound > (1 << 64):
        raise HarnessError("permutation bound is outside the uint64 range")
    threshold = ((-bound) & _UINT64_MASK) % bound
    while True:
        state, value = _splitmix64(state)
        if value >= threshold:
            return state, value % bound


def generate_permutation(count: int, seed: int) -> list[int]:
    if type(count) is not int or count <= 0:
        raise HarnessError("permutation count must be a positive integer")
    if type(seed) is not int or seed < 0 or seed > _UINT64_MASK:
        raise HarnessError("permutation seed must fit uint64")
    result = list(range(1, count + 1))
    state = seed
    for index in range(count - 1, 0, -1):
        state, selected = _uniform_bounded(state, index + 1)
        result[index], result[selected] = result[selected], result[index]
    return result


def _fnv1a64(data: bytes, state: int = _FNV1A64_OFFSET) -> int:
    for byte in data:
        state ^= byte
        state = (state * _FNV1A64_PRIME) & _UINT64_MASK
    return state


def fingerprint_integers(values: list[int]) -> str:
    state = _FNV1A64_OFFSET
    for value in values:
        if type(value) is not int or value < 0 or value > _UINT64_MASK:
            raise HarnessError("fingerprint integers must fit uint64")
        state = _fnv1a64(value.to_bytes(8, "little"), state)
    return f"{state:016x}"


def expected_value(logical_row: int) -> bytes:
    if type(logical_row) is not int or logical_row <= 0 or logical_row > 0xFFFFFFFF:
        raise HarnessError("logical row must fit a positive 32-bit value")
    return f"{logical_row:08x}".encode("ascii") + (b"0" * 248)


def _digest_tagged_key(state: int, tag: int, key: int) -> int:
    state = _fnv1a64(bytes((tag,)), state)
    return _fnv1a64(key.to_bytes(8, "little"), state)


def workload_digest(
    *,
    kind: str,
    row_count: int,
    iterations: int,
    seed: int,
) -> str:
    if kind not in {"point_present", "point_missing", "scan"}:
        raise HarnessError(f"unknown workload kind: {kind}")
    if type(row_count) is not int or row_count <= 0:
        raise HarnessError("workload row count must be positive")
    if type(iterations) is not int or iterations <= 0:
        raise HarnessError("workload iterations must be positive")
    state = _FNV1A64_OFFSET
    if kind == "scan":
        for _ in range(iterations):
            state = _fnv1a64(b"S", state)
            for logical_row in range(1, row_count + 1):
                state = _digest_tagged_key(state, ord("R"), logical_row * 2)
                state = _fnv1a64(expected_value(logical_row), state)
            state = _fnv1a64(b"D", state)
        return f"{state:016x}"

    permutation = generate_permutation(row_count, seed)
    tag = ord("P") if kind == "point_present" else ord("M")
    for index in range(iterations):
        logical_row = permutation[index % row_count]
        key = (
            logical_row * 2
            if kind == "point_present"
            else (logical_row * 2) - 1
        )
        state = _digest_tagged_key(state, tag, key)
        if kind == "point_present":
            state = _fnv1a64(expected_value(logical_row), state)
        state = _fnv1a64(b"D", state)
    return f"{state:016x}"


def median_integer(values: list[int]) -> int:
    if not values or len(values) % 2 == 0:
        raise HarnessError("integer median requires a nonempty odd-length list")
    if any(type(value) is not int for value in values):
        raise HarnessError("integer median accepts integers only")
    ordered = sorted(values)
    return ordered[len(ordered) // 2]


def exact_ratio(numerator: int, denominator: int) -> dict[str, Any]:
    if type(numerator) is not int or numerator < 0:
        raise HarnessError("ratio numerator must be a nonnegative integer")
    if type(denominator) is not int or denominator <= 0:
        raise HarnessError("ratio denominator must be a positive integer")
    divisor = math.gcd(numerator, denominator)
    reduced_numerator = numerator // divisor
    reduced_denominator = denominator // divisor
    context = decimal.Context(prec=50, rounding=decimal.ROUND_HALF_EVEN)
    decimal_value = context.divide(
        decimal.Decimal(reduced_numerator),
        decimal.Decimal(reduced_denominator),
    )
    rendered = format(decimal_value.quantize(decimal.Decimal("0.000001")), "f")
    return {
        "numerator": reduced_numerator,
        "denominator": reduced_denominator,
        "decimal": rendered,
    }


def _wait_or_kill_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        process.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def _stop_process_group(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        if process.poll() is None:
            process.wait()
        return
    except PermissionError:
        _wait_or_kill_process(process)
        return
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            if process.poll() is None:
                process.wait()
            return
        except PermissionError:
            _wait_or_kill_process(process)
            return
        time.sleep(0.01)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    if process.poll() is None:
        process.wait()


def run_bounded(
    command: list[str],
    *,
    cwd: pathlib.Path,
    timeout_seconds: float,
    stdout_limit: int,
    stderr_limit: int,
) -> ChildResult:
    if not command or any(type(argument) is not str or not argument for argument in command):
        raise HarnessError("child command must contain nonempty string arguments")
    if timeout_seconds <= 0:
        raise HarnessError("child timeout must be positive")
    if stdout_limit < 0 or stderr_limit < 0:
        raise HarnessError("child output limits must be nonnegative")
    started_ns = time.monotonic_ns()
    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
    except OSError as error:
        raise HarnessError(f"cannot launch child process: {error}") from error

    assert process.stdout is not None
    assert process.stderr is not None
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ, "stdout")
    selector.register(process.stderr, selectors.EVENT_READ, "stderr")
    stdout = bytearray()
    stderr = bytearray()
    deadline = time.monotonic() + timeout_seconds

    def fail(message: str, *, timed_out: bool) -> None:
        _stop_process_group(process)
        returncode = process.poll()
        raise ChildExecutionError(
            message,
            returncode=-1 if returncode is None else returncode,
            stdout=bytes(stdout),
            stderr=bytes(stderr),
            elapsed_ns=time.monotonic_ns() - started_ns,
            timed_out=timed_out,
        )

    try:
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                fail("child process timed out", timed_out=True)
            events = selector.select(remaining)
            if not events:
                if process.poll() is None:
                    fail("child process timed out", timed_out=True)
                continue
            for key, _ in events:
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                target = stdout if key.data == "stdout" else stderr
                limit = stdout_limit if key.data == "stdout" else stderr_limit
                if len(target) + len(chunk) > limit:
                    remaining_capacity = max(0, limit - len(target))
                    target.extend(chunk[:remaining_capacity])
                    fail(
                        f"child {key.data} exceeded the {key.data} limit",
                        timed_out=False,
                    )
                target.extend(chunk)
        remaining = max(0.0, deadline - time.monotonic())
        try:
            returncode = process.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            fail("child process timed out", timed_out=True)
    finally:
        selector.close()
        process.stdout.close()
        process.stderr.close()
    return ChildResult(
        returncode=returncode,
        stdout=bytes(stdout),
        stderr=bytes(stderr),
        elapsed_ns=time.monotonic_ns() - started_ns,
    )


def _load_pinned_sqlite(
    *,
    library_path: pathlib.Path,
    profile: dict[str, Any],
    sqlite_c_path: pathlib.Path,
    sqlite_h_path: pathlib.Path,
) -> Any:
    if __package__:
        from tools import read_compatibility
    else:
        import read_compatibility

    try:
        return read_compatibility.PinnedSqliteOracle(
            library_path,
            profile,
            sqlite_c_path,
            sqlite_h_path,
        )
    except read_compatibility.HarnessError as error:
        raise HarnessError(str(error)) from error


def _sqlite_prepare(oracle: Any, database: Any, sql: str) -> Any:
    try:
        statement, result, next_offset = oracle._prepare(
            database,
            sql.encode("utf-8"),
        )
    except Exception as error:
        if isinstance(error, HarnessError):
            raise
        raise HarnessError(f"cannot prepare fixture query: {error}") from error
    if result != _SQLITE_OK or statement.value is None or next_offset != len(
        sql.encode("utf-8")
    ):
        raise HarnessError(f"cannot prepare fixture query: {sql!r}, code={result}")
    return statement


def _sqlite_finalize(oracle: Any, statement: Any) -> None:
    result = oracle.library.sqlite3_finalize(statement)
    if result != _SQLITE_OK:
        raise HarnessError(f"sqlite3_finalize failed with code {result}")


def _sqlite_single_integer(oracle: Any, database: Any, sql: str) -> int:
    statement = _sqlite_prepare(oracle, database, sql)
    try:
        if oracle.library.sqlite3_step(statement) != _SQLITE_ROW:
            raise HarnessError(f"fixture query returned no row: {sql!r}")
        value = int(oracle.library.sqlite3_column_int64(statement, 0))
        if oracle.library.sqlite3_step(statement) != _SQLITE_DONE:
            raise HarnessError(f"fixture query returned extra rows: {sql!r}")
        return value
    finally:
        _sqlite_finalize(oracle, statement)


def _sqlite_single_text(oracle: Any, database: Any, sql: str) -> str:
    statement = _sqlite_prepare(oracle, database, sql)
    try:
        if oracle.library.sqlite3_step(statement) != _SQLITE_ROW:
            raise HarnessError(f"fixture query returned no row: {sql!r}")
        pointer = oracle.library.sqlite3_column_text(statement, 0)
        length = oracle.library.sqlite3_column_bytes(statement, 0)
        if pointer is None or length < 0:
            raise HarnessError(f"fixture query returned invalid text: {sql!r}")
        value = ctypes.string_at(pointer, length).decode("utf-8")
        if oracle.library.sqlite3_step(statement) != _SQLITE_DONE:
            raise HarnessError(f"fixture query returned extra rows: {sql!r}")
        return value
    finally:
        _sqlite_finalize(oracle, statement)


def _fixture_content_fingerprint(
    oracle: Any,
    database: Any,
    row_count: int,
) -> str:
    statement = _sqlite_prepare(oracle, database, "SELECT k,v FROM kv")
    state = _FNV1A64_OFFSET
    logical_row = 1
    try:
        while True:
            result = oracle.library.sqlite3_step(statement)
            if result == _SQLITE_DONE:
                break
            if result != _SQLITE_ROW:
                raise HarnessError(
                    f"fixture scan failed with SQLite code {result}"
                )
            key = int(oracle.library.sqlite3_column_int64(statement, 0))
            pointer = oracle.library.sqlite3_column_blob(statement, 1)
            length = oracle.library.sqlite3_column_bytes(statement, 1)
            if pointer is None or length != 256:
                raise HarnessError("fixture row contains an invalid BLOB")
            value = ctypes.string_at(pointer, length)
            expected_key = logical_row * 2
            if key != expected_key or value != expected_value(logical_row):
                raise HarnessError(
                    f"fixture row {logical_row} does not match the canonical value"
                )
            state = _fnv1a64(key.to_bytes(8, "little"), state)
            state = _fnv1a64(value, state)
            logical_row += 1
    finally:
        _sqlite_finalize(oracle, statement)
    if logical_row - 1 != row_count:
        raise HarnessError(
            f"fixture row count mismatch: expected {row_count}, got {logical_row - 1}"
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
    if fixture_id not in {"fit", "pressure"}:
        raise HarnessError("fixture ID must be fit or pressure")
    inputs = [
        profile_path,
        sqlite_library_path,
        sqlite_c_path,
        sqlite_h_path,
        sql_path,
        pathlib.Path(__file__),
    ]
    for path in inputs:
        if not path.is_file():
            raise HarnessError(f"fixture input is not a file: {path}")
    validate_new_output_path(output_path, protected_paths=inputs)
    profile = _validate_profile(
        load_json_strict(profile_path),
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
    preexisting_sidecars = [
        str(path) for path in sidecars if os.path.lexists(path)
    ]
    if preexisting_sidecars:
        raise HarnessError(
            f"fixture sidecar paths already exist: {preexisting_sidecars}"
        )

    oracle = _load_pinned_sqlite(
        library_path=sqlite_library_path,
        profile=profile,
        sqlite_c_path=sqlite_c_path,
        sqlite_h_path=sqlite_h_path,
    )
    try:
        oracle.create_database(output_path, sql)
    except Exception as error:
        if __package__:
            from tools import read_compatibility
        else:
            import read_compatibility

        if isinstance(error, read_compatibility.HarnessError):
            raise HarnessError(str(error)) from error
        raise

    present_sidecars = [str(path) for path in sidecars if os.path.lexists(path)]
    if present_sidecars:
        raise HarnessError(f"fixture left sidecar files: {present_sidecars}")
    try:
        header = output_path.read_bytes()[:100]
        size_bytes = output_path.stat().st_size
    except OSError as error:
        raise HarnessError(f"cannot inspect generated fixture: {error}") from error
    if len(header) != 100 or header[:16] != b"SQLite format 3\0":
        raise HarnessError("generated fixture does not have a SQLite header")
    encoded_page_size = int.from_bytes(header[16:18], "big")
    page_size = 65536 if encoded_page_size == 1 else encoded_page_size
    if page_size != 4096 or size_bytes % page_size != 0:
        raise HarnessError("generated fixture has an invalid page layout")
    page_count = size_bytes // page_size
    header_page_count = int.from_bytes(header[28:32], "big")
    if header_page_count != page_count:
        raise HarnessError("generated fixture header page count is not canonical")

    database, result = oracle._open(output_path, _SQLITE_OPEN_READONLY)
    if result != _SQLITE_OK:
        try:
            oracle._close(database)
        finally:
            raise HarnessError(
                f"cannot reopen generated fixture with SQLite code {result}"
            )
    try:
        oracle._configure_connection(database)
        integrity = _sqlite_single_text(
            oracle,
            database,
            "PRAGMA integrity_check",
        )
        if integrity != "ok":
            raise HarnessError(f"generated fixture integrity check failed: {integrity}")
        journal_mode = _sqlite_single_text(
            oracle,
            database,
            "PRAGMA journal_mode",
        ).lower()
        if journal_mode != "delete":
            raise HarnessError(
                f"generated fixture journal mode is {journal_mode}, not delete"
            )
        schema_sql = _sqlite_single_text(
            oracle,
            database,
            "SELECT sql FROM sqlite_schema "
            "WHERE type='table' AND name='kv'",
        )
        column_signature = _sqlite_single_text(
            oracle,
            database,
            "SELECT group_concat(signature, '|') FROM ("
            "SELECT cid||':'||name||':'||type||':'||\"notnull\"||':'||"
            "coalesce(dflt_value,'NULL')||':'||pk||':'||hidden AS signature "
            "FROM pragma_table_xinfo('kv') ORDER BY cid)",
        )
        if (
            schema_sql
            != "CREATE TABLE kv(\n"
            "  k INTEGER PRIMARY KEY,\n"
            "  v BLOB NOT NULL\n"
            ")"
            or column_signature
            != "0:k:INTEGER:0:NULL:1:0|1:v:BLOB:1:NULL:0:0"
            or _sqlite_single_integer(
                oracle,
                database,
                "SELECT count(*) FROM sqlite_schema "
                "WHERE name NOT LIKE 'sqlite_%'",
            )
            != 1
            or _sqlite_single_text(oracle, database, "PRAGMA encoding")
            != "UTF-8"
            or _sqlite_single_integer(oracle, database, "PRAGMA auto_vacuum")
            != 0
            or _sqlite_single_integer(
                oracle,
                database,
                "PRAGMA application_id",
            )
            != 1_297_305_936
            or _sqlite_single_integer(oracle, database, "PRAGMA user_version")
            != 1
        ):
            raise HarnessError(
                "generated fixture schema contract is not canonical"
            )
        effective_page_size = _sqlite_single_integer(
            oracle,
            database,
            "PRAGMA page_size",
        )
        effective_page_count = _sqlite_single_integer(
            oracle,
            database,
            "PRAGMA page_count",
        )
        row_count = _sqlite_single_integer(
            oracle,
            database,
            "SELECT count(*) FROM kv",
        )
        expected_rows = 4096 if fixture_id == "fit" else 65536
        if (
            effective_page_size != page_size
            or effective_page_count != page_count
            or row_count != expected_rows
        ):
            raise HarnessError("generated fixture metadata does not match its role")
        if fixture_id == "fit" and page_count >= CACHE_PAGES:
            raise HarnessError("fit fixture does not fit in the 512-page cache")
        if fixture_id == "pressure" and page_count <= CACHE_PAGES:
            raise HarnessError(
                "pressure fixture does not exceed the 512-page cache"
            )
        content_fingerprint = _fixture_content_fingerprint(
            oracle,
            database,
            row_count,
        )
    finally:
        oracle._close(database)

    seed = (
        0x9E3779B97F4A7C15
        if fixture_id == "fit"
        else 0xD1B54A32D192ED03
    )
    permutation = generate_permutation(row_count, seed)
    present_keys = [logical_row * 2 for logical_row in permutation]
    missing_keys = [(logical_row * 2) - 1 for logical_row in permutation]
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
        "content_fingerprint": content_fingerprint,
        "present_order_fingerprint": fingerprint_integers(present_keys),
        "missing_order_fingerprint": fingerprint_integers(missing_keys),
    }


def _validate_profile(
    profile: Any,
    *,
    label: str,
) -> dict[str, Any]:
    _require_type(profile, dict, label)
    sqlite = profile.get("sqlite")
    build = profile.get("build")
    _require_type(sqlite, dict, f"{label}.sqlite")
    _require_type(build, dict, f"{label}.build")
    if sqlite.get("version") != SQLITE_VERSION:
        raise HarnessError(f"{label} SQLite version is not pinned to {SQLITE_VERSION}")
    if sqlite.get("source_id") != SQLITE_SOURCE_ID:
        raise HarnessError(f"{label} SQLite source ID is not pinned")
    _require_sha256(
        sqlite.get("sqlite3_c_sha256"),
        f"{label}.sqlite.sqlite3_c_sha256",
    )
    _require_sha256(
        sqlite.get("sqlite3_h_sha256"),
        f"{label}.sqlite.sqlite3_h_sha256",
    )
    compile_options = build.get("semantic_compile_options")
    _require_type(compile_options, list, f"{label}.build.semantic_compile_options")
    if (
        not compile_options
        or any(type(option) is not str or not option for option in compile_options)
        or compile_options != sorted(set(compile_options))
    ):
        raise HarnessError(
            f"{label}.build.semantic_compile_options must be sorted and unique"
        )
    return profile


def _validate_work(value: Any, label: str) -> dict[str, Any]:
    _require_type(value, dict, label)
    _require_exact_keys(value, _WORK_KEYS, label)
    for key in (
        "operations",
        "items",
        "rows",
        "bytes",
        "result_hits",
        "result_misses",
    ):
        _require_integer(value[key], f"{label}.{key}")
    _require_fingerprint(value["digest"], f"{label}.digest")
    if value["result_hits"] + value["result_misses"] != value["operations"]:
        raise HarnessError(
            f"{label} result hits plus misses must equal operations"
        )
    return value


def _validate_fixture(
    value: Any,
    *,
    repository_root: pathlib.Path,
    label: str,
) -> dict[str, Any]:
    _require_type(value, dict, label)
    _require_exact_keys(value, _FIXTURE_KEYS, label)
    fixture_id = _require_identifier(value["id"], f"{label}.id")
    if fixture_id not in {"fit", "pressure"}:
        raise HarnessError(f"{label}.id must be fit or pressure")
    database = _resolve_repository_file(
        repository_root,
        value["path"],
        f"{label}.path",
    )
    sql = _resolve_repository_file(
        repository_root,
        value["sql_path"],
        f"{label}.sql_path",
    )
    expected_database_sha = _require_sha256(
        value["sha256"],
        f"{label}.sha256",
    )
    if _sha256(database) != expected_database_sha:
        raise HarnessError(f"{label} fixture SHA-256 does not match {database}")
    expected_sql_sha = _require_sha256(
        value["sql_sha256"],
        f"{label}.sql_sha256",
    )
    if _sha256(sql) != expected_sql_sha:
        raise HarnessError(f"{label} SQL SHA-256 does not match {sql}")
    if _require_integer(value["size_bytes"], f"{label}.size_bytes", minimum=1) != (
        database.stat().st_size
    ):
        raise HarnessError(f"{label}.size_bytes does not match the fixture")
    if value["page_size"] != 4096:
        raise HarnessError(f"{label}.page_size must be 4096")
    page_count = _require_integer(
        value["page_count"],
        f"{label}.page_count",
        minimum=1,
    )
    row_count = _require_integer(
        value["row_count"],
        f"{label}.row_count",
        minimum=1,
    )
    if value["value_size"] != 256:
        raise HarnessError(f"{label}.value_size must be 256")
    if fixture_id == "fit":
        if row_count != 4096 or page_count >= CACHE_PAGES:
            raise HarnessError(
                "fit fixture must have 4096 rows and fewer than 512 pages"
            )
    elif row_count != 65536 or page_count <= CACHE_PAGES:
        raise HarnessError(
            "pressure fixture must have 65536 rows and more than 512 pages"
        )
    for key in (
        "content_fingerprint",
        "present_order_fingerprint",
        "missing_order_fingerprint",
    ):
        _require_fingerprint(value[key], f"{label}.{key}")
    return value


def _expected_case_contract(
    case_id: str,
) -> tuple[str, str, str, int, int, int]:
    fixture = "pressure" if case_id.endswith("-pressure") else "fit"
    row_count = 65536 if fixture == "pressure" else 4096
    if case_id.startswith("point-present"):
        return (
            fixture,
            "point_present",
            "SELECT v FROM kv WHERE k=?1",
            row_count,
            1_048_576,
            row_count,
        )
    if case_id.startswith("point-missing"):
        return (
            fixture,
            "point_missing",
            "SELECT v FROM kv WHERE k=?1",
            row_count,
            1_048_576,
            row_count,
        )
    return (
        fixture,
        "scan",
        "SELECT k,v FROM kv",
        1,
        64 if fixture == "pressure" else 1024,
        1,
    )


def _validate_case(value: Any, label: str) -> dict[str, Any]:
    _require_type(value, dict, label)
    _require_exact_keys(value, _CASE_KEYS, label)
    case_id = _require_identifier(value["id"], f"{label}.id")
    if case_id not in EXPECTED_CASE_IDS:
        raise HarnessError(f"{label}.id is not a pinned case ID")
    (
        fixture,
        kind,
        sql,
        warmup_iterations,
        measured_iterations,
        diagnostic_iterations,
    ) = _expected_case_contract(case_id)
    expected_unit = "traversal" if kind == "scan" else "lookup"
    expected_items = 65536 if case_id == "scan-ipk-pressure" else 4096
    if kind != "scan":
        expected_items = 1
    exact = {
        "fixture": fixture,
        "kind": kind,
        "sql": sql,
        "primary_unit": expected_unit,
        "warmup_iterations": warmup_iterations,
        "measured_iterations": measured_iterations,
        "diagnostic_iterations": diagnostic_iterations,
        "items_per_iteration": expected_items,
    }
    for key, expected in exact.items():
        if value[key] != expected:
            raise HarnessError(
                f"{label}.{key} must be {expected!r} for {case_id}"
            )
    expected_groups = value["expected"]
    _require_type(expected_groups, dict, f"{label}.expected")
    _require_exact_keys(
        expected_groups,
        _EXPECTED_GROUP_KEYS,
        f"{label}.expected",
    )
    for group in sorted(_EXPECTED_GROUP_KEYS):
        _validate_work(
            expected_groups[group],
            f"{label}.expected.{group}",
        )
    measured = expected_groups["measured"]
    if measured["operations"] != measured_iterations:
        raise HarnessError(
            f"{label}.expected.measured operations do not match iterations"
        )
    if measured["items"] != measured_iterations * expected_items:
        raise HarnessError(
            f"{label}.expected.measured items do not match the case unit"
        )
    return value


def validate_workload_manifest(
    value: Any,
    *,
    repository_root: pathlib.Path,
    manifest_path: pathlib.Path,
) -> dict[str, Any]:
    _require_type(value, dict, "workload manifest")
    _require_exact_keys(value, _MANIFEST_KEYS, "workload manifest")
    if value["schema_version"] != SCHEMA_VERSION:
        raise HarnessError("workload manifest schema_version must be 1")
    if value["workload_semantics_version"] != WORKLOAD_SEMANTICS_VERSION:
        raise HarnessError(
            "workload manifest workload_semantics_version must be 1"
        )
    manifest_resolved = manifest_path.resolve()
    root_resolved = repository_root.resolve()
    try:
        manifest_resolved.relative_to(root_resolved)
    except ValueError as error:
        raise HarnessError("workload manifest path escapes the repository") from error

    profile_path = _resolve_repository_file(
        repository_root,
        value["sqlite_profile"],
        "workload manifest.sqlite_profile",
    )
    profile = _validate_profile(
        load_json_strict(profile_path),
        label="SQLite profile",
    )
    semantic_compile_options = value["sqlite_semantic_compile_options"]
    _require_type(
        semantic_compile_options,
        list,
        "workload manifest.sqlite_semantic_compile_options",
    )
    if semantic_compile_options != profile["build"]["semantic_compile_options"]:
        raise HarnessError(
            "workload manifest SQLite semantic compile options "
            "do not match the pinned profile"
        )

    configuration = value["configuration"]
    _require_type(configuration, dict, "workload manifest.configuration")
    _require_exact_keys(
        configuration,
        _CONFIGURATION_KEYS,
        "workload manifest.configuration",
    )
    expected_configuration = {
        "page_size": 4096,
        "cache_pages": 512,
        "mmap_bytes": 0,
        "temp_store": "memory",
        "synchronous": "full",
        "journal_mode": "delete",
        "query_only": True,
        "thread_mode": "single",
    }
    if configuration != expected_configuration:
        raise HarnessError("workload manifest configuration is not pinned")

    permutation = value["permutation"]
    _require_type(permutation, dict, "workload manifest.permutation")
    _require_exact_keys(
        permutation,
        _PERMUTATION_KEYS,
        "workload manifest.permutation",
    )
    expected_permutation = {
        "algorithm": "splitmix64-rejection-fisher-yates-v1",
        "fingerprint": "fnv1a64-v1",
        "fit_seed": "9e3779b97f4a7c15",
        "pressure_seed": "d1b54a32d192ed03",
    }
    if permutation != expected_permutation:
        raise HarnessError("workload manifest permutation contract is not pinned")

    rounds = value["rounds"]
    _require_type(rounds, list, "workload manifest.rounds")
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
    if rounds != expected_rounds:
        raise HarnessError("workload manifest rounds are not the pinned schedule")
    for index, round_value in enumerate(rounds):
        _require_type(round_value, dict, f"workload manifest.rounds[{index}]")
        _require_exact_keys(
            round_value,
            _ROUND_KEYS,
            f"workload manifest.rounds[{index}]",
        )

    fixtures = value["fixtures"]
    _require_type(fixtures, list, "workload manifest.fixtures")
    if len(fixtures) != 2:
        raise HarnessError("workload manifest must contain two fixtures")
    validated_fixtures = [
        _validate_fixture(
            fixture,
            repository_root=repository_root,
            label=f"workload manifest.fixtures[{index}]",
        )
        for index, fixture in enumerate(fixtures)
    ]
    if [fixture["id"] for fixture in validated_fixtures] != ["fit", "pressure"]:
        raise HarnessError("workload manifest fixture IDs must be fit then pressure")

    cases = value["cases"]
    _require_type(cases, list, "workload manifest.cases")
    validated_cases = [
        _validate_case(case, f"workload manifest.cases[{index}]")
        for index, case in enumerate(cases)
    ]
    case_ids = tuple(case["id"] for case in validated_cases)
    if case_ids != EXPECTED_CASE_IDS:
        raise HarnessError(
            f"workload manifest case IDs must be {list(EXPECTED_CASE_IDS)}"
        )

    guard = value["guard"]
    _require_type(guard, dict, "workload manifest.guard")
    _require_exact_keys(guard, _GUARD_KEYS, "workload manifest.guard")
    for key in sorted(_GUARD_KEYS):
        ratio = guard[key]
        _require_type(ratio, dict, f"workload manifest.guard.{key}")
        _require_exact_keys(
            ratio,
            _RATIO_LIMIT_KEYS,
            f"workload manifest.guard.{key}",
        )
        if ratio != {"numerator": 10, "denominator": 1}:
            raise HarnessError(
                f"workload manifest.guard.{key} must be exactly 10/1"
            )
    return value


def _case_by_id(
    workload_manifest: dict[str, Any],
    case_id: str,
) -> dict[str, Any]:
    cases = workload_manifest.get("cases")
    _require_type(cases, list, "workload manifest.cases")
    for case in cases:
        if type(case) is dict and case.get("id") == case_id:
            return case
    raise HarnessError(f"unknown workload case: {case_id}")


def _validate_build(
    value: Any,
    *,
    instrumentation: bool,
    label: str,
) -> None:
    _require_type(value, dict, label)
    _require_exact_keys(value, _BUILD_KEYS, label)
    if value["build_type"] != "Release":
        raise HarnessError(f"{label} must describe a Release build")
    for key in ("instrumentation", "sanitizers", "coverage"):
        _require_type(value[key], bool, f"{label}.{key}")
    if value["instrumentation"] is not instrumentation:
        requirement = "instrumented" if instrumentation else "uninstrumented"
        raise HarnessError(f"{label} must describe an {requirement} build")
    if value["sanitizers"] or value["coverage"]:
        raise HarnessError(f"{label} cannot describe sanitizers or coverage")


def _validate_sqlite(
    value: Any,
    *,
    expected_semantic_options: list[str],
    label: str,
) -> None:
    _require_type(value, dict, label)
    _require_exact_keys(value, _SQLITE_KEYS, label)
    if value["version"] != SQLITE_VERSION:
        raise HarnessError(f"{label}.version is not pinned")
    if value["source_id"] != SQLITE_SOURCE_ID:
        raise HarnessError(f"{label}.source_id is not pinned")
    compile_options = value["compile_options"]
    _require_type(compile_options, list, f"{label}.compile_options")
    if (
        not compile_options
        or any(type(option) is not str or not option for option in compile_options)
        or compile_options != sorted(set(compile_options))
    ):
        raise HarnessError(
            f"{label}.compile_options must be nonempty, sorted, and unique"
        )
    if compile_options != expected_semantic_options:
        raise HarnessError(
            f"{label} semantic compile options do not match the pinned profile"
        )


def _validate_configuration(
    value: Any,
    workload_manifest: dict[str, Any],
    label: str,
) -> None:
    _require_type(value, dict, label)
    expected = workload_manifest.get("configuration")
    if value != expected:
        raise HarnessError(f"{label} does not match the workload manifest")


def _validate_completion(
    value: Any,
    label: str,
    expected: dict[str, Any],
) -> None:
    _require_type(value, dict, label)
    _require_exact_keys(value, _COMPLETION_KEYS, label)
    if value["status"] != "complete":
        raise HarnessError(f"{label} completion status must be complete")
    for key in _COMPLETION_KEYS - {"status"}:
        _require_integer(value[key], f"{label}.{key}", minimum=1)
    if value != expected:
        raise HarnessError(f"{label} completion counters are invalid")


def _expected_completion(
    case: dict[str, Any],
    *,
    mode: str,
) -> dict[str, Any]:
    if mode == "baseline":
        statement_prepares = 3
        statement_resets = case["warmup_iterations"] + (
            case["measured_iterations"] * 3
        )
    elif mode == "smoke":
        statement_prepares = 3
        statement_resets = case["warmup_iterations"] + 1
    elif mode == "diagnostic":
        statement_prepares = 4
        statement_resets = (
            case["warmup_iterations"] + case["diagnostic_iterations"]
        )
    else:
        raise HarnessError(f"unsupported completion mode: {mode}")
    statement_resets += 2
    return {
        "status": "complete",
        "session_opens": 1,
        "statement_prepares": statement_prepares,
        "statement_finalizes": statement_prepares,
        "statement_resets": statement_resets,
        "pre_verifications": 1,
        "post_verifications": 1,
    }


def _validate_expected_work(
    actual: Any,
    expected: dict[str, Any],
    label: str,
) -> None:
    _validate_work(actual, label)
    if actual != expected:
        raise HarnessError(f"{label} does not match expected measured work")


def _smoke_work(
    workload_manifest: dict[str, Any],
    case: dict[str, Any],
) -> dict[str, Any]:
    fixture_id = case["fixture"]
    fixtures = workload_manifest["fixtures"]
    fixture = next(
        candidate for candidate in fixtures if candidate["id"] == fixture_id
    )
    row_count = fixture["row_count"]
    seed = int(workload_manifest["permutation"][f"{fixture_id}_seed"], 16)
    kind = case["kind"]
    if kind == "point_present":
        return {
            "operations": 1,
            "items": 1,
            "rows": 1,
            "bytes": 256,
            "result_hits": 1,
            "result_misses": 0,
            "digest": workload_digest(
                kind=kind,
                row_count=row_count,
                iterations=1,
                seed=seed,
            ),
        }
    if kind == "point_missing":
        return {
            "operations": 1,
            "items": 1,
            "rows": 0,
            "bytes": 0,
            "result_hits": 0,
            "result_misses": 1,
            "digest": workload_digest(
                kind=kind,
                row_count=row_count,
                iterations=1,
                seed=seed,
            ),
        }
    return case["expected"]["verification"]


def validate_raw_timing_report(
    value: Any,
    *,
    workload_manifest: dict[str, Any],
    expected_engine: str,
    expected_case: str,
    expected_run_kind: str = "baseline",
) -> dict[str, Any]:
    _require_type(value, dict, "timing report")
    _require_exact_keys(value, _RAW_TIMING_KEYS, "timing report")
    if value["schema_version"] != SCHEMA_VERSION:
        raise HarnessError("timing report schema_version must be 1")
    if value["completion_schema_version"] != COMPLETION_SCHEMA_VERSION:
        raise HarnessError("timing report completion_schema_version must be 1")
    if value["workload_semantics_version"] != WORKLOAD_SEMANTICS_VERSION:
        raise HarnessError("timing report workload_semantics_version must be 1")
    if expected_run_kind not in {"baseline", "smoke"}:
        raise HarnessError("expected run kind must be baseline or smoke")
    if value["mode"] != "timing" or value["run_kind"] != expected_run_kind:
        raise HarnessError(
            f"timing report mode must be {expected_run_kind} timing"
        )
    if expected_engine not in {"modern", "sqlite"}:
        raise HarnessError("expected engine must be modern or sqlite")
    if value["engine"] != expected_engine:
        raise HarnessError("timing report engine does not match the invocation")
    if value["case"] != expected_case:
        raise HarnessError("timing report case does not match the invocation")
    case = _case_by_id(workload_manifest, expected_case)
    _validate_build(
        value["build"],
        instrumentation=False,
        label="timing report.build",
    )
    _validate_sqlite(
        value["sqlite"],
        expected_semantic_options=workload_manifest[
            "sqlite_semantic_compile_options"
        ],
        label="timing report.sqlite",
    )
    _validate_configuration(
        value["effective_configuration"],
        workload_manifest,
        "timing report.effective_configuration",
    )
    timer = value["timer"]
    _require_type(timer, dict, "timing report.timer")
    _require_exact_keys(timer, _TIMER_KEYS, "timing report.timer")
    if timer != {
        "wall": "steady_clock",
        "cpu": "CLOCK_PROCESS_CPUTIME_ID",
    }:
        raise HarnessError("timing report timer identities are invalid")

    expected_groups = case["expected"]
    _validate_expected_work(
        value["warmup"],
        expected_groups["warmup"],
        "timing report.warmup",
    )
    repetitions = value["repetitions"]
    _require_type(repetitions, list, "timing report.repetitions")
    expected_repetition_count = 3 if expected_run_kind == "baseline" else 1
    if len(repetitions) != expected_repetition_count:
        raise HarnessError(
            "timing report repetitions contain the wrong number of rows"
        )
    expected_measured = (
        expected_groups["measured"]
        if expected_run_kind == "baseline"
        else _smoke_work(workload_manifest, case)
    )
    indexes = []
    for position, repetition in enumerate(repetitions):
        label = f"timing report.repetitions[{position}]"
        _require_type(repetition, dict, label)
        _require_exact_keys(repetition, _REPETITION_KEYS, label)
        indexes.append(_require_integer(repetition["index"], f"{label}.index"))
        wall_ns = _require_integer(
            repetition["wall_ns"],
            f"{label}.wall_ns",
            minimum=1,
        )
        _require_integer(
            repetition["cpu_ns"],
            f"{label}.cpu_ns",
            minimum=1,
        )
        if expected_run_kind == "baseline" and wall_ns < MINIMUM_WALL_NS:
            raise HarnessError(
                f"{label} is below the {MINIMUM_WALL_NS} minimum wall time"
            )
        work = {key: repetition[key] for key in _WORK_KEYS}
        _validate_expected_work(
            work,
            expected_measured,
            f"{label} measured work",
        )
    if indexes != list(range(expected_repetition_count)):
        raise HarnessError(
            "timing report repetition indexes are incomplete or duplicated"
        )
    _validate_completion(
        value["completion"],
        "timing report.completion",
        _expected_completion(case, mode=expected_run_kind),
    )
    return value


def _validate_counter_object(
    value: Any,
    *,
    expected_names: tuple[str, ...],
    label: str,
) -> dict[str, int]:
    _require_type(value, dict, label)
    if set(value) != set(expected_names):
        family = "Modern" if expected_names == MODERN_COUNTER_NAMES else "SQLite"
        raise HarnessError(f"{family} counter names are invalid")
    for name in expected_names:
        _require_integer(value[name], f"{label}.{name}")
    return value


def validate_raw_diagnostic_report(
    value: Any,
    *,
    workload_manifest: dict[str, Any],
    expected_engine: str,
    expected_case: str,
) -> dict[str, Any]:
    _require_type(value, dict, "diagnostic report")
    _require_exact_keys(value, _RAW_DIAGNOSTIC_KEYS, "diagnostic report")
    if value["schema_version"] != SCHEMA_VERSION:
        raise HarnessError("diagnostic report schema_version must be 1")
    if value["completion_schema_version"] != COMPLETION_SCHEMA_VERSION:
        raise HarnessError("diagnostic report completion_schema_version must be 1")
    if value["diagnostic_schema_version"] != DIAGNOSTIC_SCHEMA_VERSION:
        raise HarnessError("diagnostic report diagnostic_schema_version must be 1")
    if value["workload_semantics_version"] != WORKLOAD_SEMANTICS_VERSION:
        raise HarnessError("diagnostic report workload_semantics_version must be 1")
    if value["mode"] != "diagnostic":
        raise HarnessError("diagnostic report mode must be diagnostic")
    if value["engine"] != expected_engine:
        raise HarnessError("diagnostic report engine does not match the invocation")
    if value["case"] != expected_case:
        raise HarnessError("diagnostic report case does not match the invocation")
    case = _case_by_id(workload_manifest, expected_case)
    _validate_build(
        value["build"],
        instrumentation=True,
        label="diagnostic report.build",
    )
    _validate_sqlite(
        value["sqlite"],
        expected_semantic_options=workload_manifest[
            "sqlite_semantic_compile_options"
        ],
        label="diagnostic report.sqlite",
    )
    _validate_configuration(
        value["effective_configuration"],
        workload_manifest,
        "diagnostic report.effective_configuration",
    )

    counters = value["counters"]
    _require_type(counters, dict, "diagnostic report.counters")
    _require_exact_keys(
        counters,
        _COUNTER_GROUP_KEYS,
        "diagnostic report.counters",
    )
    if expected_engine == "modern":
        modern = _validate_counter_object(
            counters["modern"],
            expected_names=MODERN_COUNTER_NAMES,
            label="diagnostic report.counters.modern",
        )
        if counters["sqlite"] != {}:
            raise HarnessError("Modern diagnostics must not publish SQLite counters")
        if modern["pages_written"] != 0:
            raise HarnessError("Modern diagnostic pages_written must be zero")
        if any(
            modern[name] == 0
            for name in (
                "allocations",
                "vfs_calls",
                "cache_hits",
                "vm_instructions",
                "planner_work",
            )
        ):
            raise HarnessError("Modern diagnostic fixed-work counters must be nonzero")
        if modern["pages_read"] != modern["cache_misses"]:
            raise HarnessError(
                "Modern diagnostic page reads and cache misses must match"
            )
        if case["kind"].startswith("point_") and modern["btree_comparisons"] == 0:
            raise HarnessError(
                "Modern point diagnostic must record B-tree comparisons"
            )
        misses = modern["cache_misses"]
    elif expected_engine == "sqlite":
        sqlite = _validate_counter_object(
            counters["sqlite"],
            expected_names=SQLITE_COUNTER_NAMES,
            label="diagnostic report.counters.sqlite",
        )
        if counters["modern"] != {}:
            raise HarnessError("SQLite diagnostics must not publish Modern counters")
        if sqlite["cache_writes"] != 0:
            raise HarnessError("SQLite diagnostic cache_writes must be zero")
        if any(
            sqlite[name] == 0
            for name in (
                "cache_hits",
                "cache_bytes_current",
                "vm_steps",
                "statement_runs",
                "malloc_count_current",
                "malloc_count_highwater",
                "malloc_size_highwater",
            )
        ):
            raise HarnessError("SQLite diagnostic fixed-work counters must be nonzero")
        if case["kind"] == "scan" and sqlite["fullscan_steps"] == 0:
            raise HarnessError("SQLite scan diagnostic must record full-scan steps")
        if case["kind"] != "scan" and sqlite["fullscan_steps"] != 0:
            raise HarnessError(
                "SQLite point diagnostic cannot record full-scan steps"
            )
        misses = sqlite["cache_misses"]
    else:
        raise HarnessError("expected engine must be modern or sqlite")

    fixture = case["fixture"]
    if fixture == "fit" and misses != 0:
        raise HarnessError("fit diagnostic must have zero measured cache misses")
    if fixture == "pressure" and misses == 0:
        raise HarnessError("pressure diagnostic must have measured cache misses")

    _validate_expected_work(
        value["work"],
        case["expected"]["diagnostic"],
        "diagnostic report.work",
    )
    _validate_completion(
        value["completion"],
        "diagnostic report.completion",
        _expected_completion(case, mode="diagnostic"),
    )
    return value


def _ratio_at_most(
    ratio: dict[str, Any],
    limit: dict[str, Any],
) -> bool:
    return (
        ratio["numerator"] * limit["denominator"]
        <= limit["numerator"] * ratio["denominator"]
    )


def _ratio_order_key(ratio: dict[str, Any]) -> fractions.Fraction:
    return fractions.Fraction(
        ratio["numerator"],
        ratio["denominator"],
    )


def _engine_aggregate(
    *,
    reports: list[tuple[int, dict[str, Any]]],
) -> dict[str, Any]:
    repetitions = []
    rounds = []
    for round_index, report in reports:
        round_repetitions = report["repetitions"]
        for repetition in round_repetitions:
            repetitions.append({"round": round_index, **repetition})
        rounds.append(
            {
                "index": round_index,
                "wall_median_ns": median_integer(
                    [item["wall_ns"] for item in round_repetitions]
                ),
                "cpu_median_ns": median_integer(
                    [item["cpu_ns"] for item in round_repetitions]
                ),
            }
        )
    return {
        "repetitions": repetitions,
        "rounds": rounds,
        "wall_median_ns": median_integer(
            [item["wall_ns"] for item in repetitions]
        ),
        "cpu_median_ns": median_integer(
            [item["cpu_ns"] for item in repetitions]
        ),
    }


def _case_aggregate(
    *,
    case_id: str,
    reports: dict[tuple[int, str, str], dict[str, Any]],
    rounds: list[dict[str, Any]],
    guard: dict[str, Any],
) -> dict[str, Any]:
    engines = {}
    for engine in ("modern", "sqlite"):
        engine_reports = [
            (round_definition["index"], reports[(round_definition["index"], case_id, engine)])
            for round_definition in rounds
        ]
        engines[engine] = _engine_aggregate(reports=engine_reports)

    aggregate_ratios = {
        "wall": exact_ratio(
            engines["modern"]["wall_median_ns"],
            engines["sqlite"]["wall_median_ns"],
        ),
        "cpu": exact_ratio(
            engines["modern"]["cpu_median_ns"],
            engines["sqlite"]["cpu_median_ns"],
        ),
    }
    round_ratios = []
    for round_definition in rounds:
        round_index = round_definition["index"]
        modern_round = engines["modern"]["rounds"][round_index]
        sqlite_round = engines["sqlite"]["rounds"][round_index]
        round_ratios.append(
            {
                "index": round_index,
                "wall": exact_ratio(
                    modern_round["wall_median_ns"],
                    sqlite_round["wall_median_ns"],
                ),
                "cpu": exact_ratio(
                    modern_round["cpu_median_ns"],
                    sqlite_round["cpu_median_ns"],
                ),
            }
        )

    bounds = {}
    for metric in ("wall", "cpu"):
        metric_ratios = [item[metric] for item in round_ratios]
        bounds[metric] = {
            "minimum": min(metric_ratios, key=_ratio_order_key),
            "maximum": max(metric_ratios, key=_ratio_order_key),
        }

    guard_passed = (
        _ratio_at_most(aggregate_ratios["wall"], guard["maximum_wall_ratio"])
        and _ratio_at_most(aggregate_ratios["cpu"], guard["maximum_cpu_ratio"])
        and all(
            _ratio_at_most(item["wall"], guard["maximum_wall_ratio"])
            and _ratio_at_most(item["cpu"], guard["maximum_cpu_ratio"])
            for item in round_ratios
        )
    )
    return {
        "case": case_id,
        "engines": engines,
        "ratios": {
            "aggregate": aggregate_ratios,
            "rounds": round_ratios,
            "bounds": bounds,
        },
        "guard_passed": guard_passed,
    }


def build_aggregate_report(
    *,
    workload_manifest: dict[str, Any],
    timing_reports: dict[tuple[int, str, str], dict[str, Any]],
    run_manifest_path: str,
    run_manifest_sha256: str,
) -> dict[str, Any]:
    _require_sha256(run_manifest_sha256, "run manifest SHA-256")
    if (
        type(run_manifest_path) is not str
        or not run_manifest_path
        or pathlib.PurePosixPath(run_manifest_path).is_absolute()
    ):
        raise HarnessError("run manifest path must be a relative POSIX path")

    rounds = workload_manifest.get("rounds")
    cases = workload_manifest.get("cases")
    _require_type(rounds, list, "workload manifest.rounds")
    _require_type(cases, list, "workload manifest.cases")
    expected_keys = {
        (round_definition["index"], case["id"], engine)
        for round_definition in rounds
        for case in cases
        for engine in ("modern", "sqlite")
    }
    if set(timing_reports) != expected_keys:
        raise HarnessError("timing report matrix is incomplete or duplicated")
    for round_index, case_id, engine in sorted(expected_keys):
        validate_raw_timing_report(
            timing_reports[(round_index, case_id, engine)],
            workload_manifest=workload_manifest,
            expected_engine=engine,
            expected_case=case_id,
            expected_run_kind="baseline",
        )

    case_reports = [
        _case_aggregate(
            case_id=case["id"],
            reports=timing_reports,
            rounds=rounds,
            guard=workload_manifest["guard"],
        )
        for case in cases
    ]
    guard_passed = all(case["guard_passed"] for case in case_reports)
    return {
        "schema_version": SCHEMA_VERSION,
        "workload_semantics_version": WORKLOAD_SEMANTICS_VERSION,
        "completion_schema_version": COMPLETION_SCHEMA_VERSION,
        "diagnostic_schema_version": DIAGNOSTIC_SCHEMA_VERSION,
        "status": "pass" if guard_passed else "fail",
        "run_manifest": {
            "path": run_manifest_path,
            "sha256": run_manifest_sha256,
        },
        "guard": workload_manifest["guard"],
        "cases": case_reports,
        "guard_passed": guard_passed,
    }


def validate_aggregate_report(
    value: Any,
    *,
    workload_manifest: dict[str, Any],
    timing_reports: dict[tuple[int, str, str], dict[str, Any]],
    expected_run_manifest_path: str,
    expected_run_manifest_sha256: str,
) -> dict[str, Any]:
    _require_type(value, dict, "aggregate report")
    _require_exact_keys(value, _AGGREGATE_KEYS, "aggregate report")
    expected = build_aggregate_report(
        workload_manifest=workload_manifest,
        timing_reports=timing_reports,
        run_manifest_path=expected_run_manifest_path,
        run_manifest_sha256=expected_run_manifest_sha256,
    )
    if value != expected:
        raise HarnessError("aggregate report derived values are inconsistent")
    return value


def enforce_performance_guard(aggregate_report: dict[str, Any]) -> None:
    if aggregate_report.get("guard_passed") is not True:
        raise PerformanceMismatch("read baseline exceeds the severe-regression guard")


def _canonical_json_bytes(value: Any) -> bytes:
    try:
        text = json.dumps(
            value,
            allow_nan=False,
            indent=2,
            sort_keys=True,
        )
    except (TypeError, ValueError) as error:
        raise HarnessError(f"cannot serialize strict JSON: {error}") from error
    return (text + "\n").encode("utf-8")


def _write_new_bytes(path: pathlib.Path, data: bytes) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as output:
            output.write(data)
    except OSError as error:
        raise HarnessError(f"cannot create artifact {path}: {error}") from error


def _write_new_json(path: pathlib.Path, value: Any) -> None:
    _write_new_bytes(path, _canonical_json_bytes(value))


def _repository_relative_path(
    path: pathlib.Path,
    repository_root: pathlib.Path,
    label: str,
) -> str:
    try:
        relative = path.resolve().relative_to(repository_root.resolve())
    except ValueError as error:
        raise HarnessError(f"{label} must be inside the repository") from error
    if not relative.parts:
        raise HarnessError(f"{label} cannot be the repository root")
    return relative.as_posix()


def _safe_artifact_path(
    baseline_path: pathlib.Path,
    relative_path: Any,
    label: str,
) -> pathlib.Path:
    value = _require_string(relative_path, label)
    pure = pathlib.PurePosixPath(value)
    if pure.is_absolute() or not pure.parts or ".." in pure.parts:
        raise HarnessError(f"{label} must be a safe relative POSIX path")
    candidate = baseline_path.joinpath(*pure.parts)
    try:
        candidate.resolve().relative_to(baseline_path.resolve())
    except ValueError as error:
        raise HarnessError(f"{label} escapes the baseline directory") from error
    return candidate


def _artifact_reference(
    path: pathlib.Path,
    baseline_path: pathlib.Path,
) -> dict[str, Any]:
    try:
        relative = path.resolve().relative_to(baseline_path.resolve()).as_posix()
        size_bytes = path.stat().st_size
    except (OSError, ValueError) as error:
        raise HarnessError(f"cannot reference artifact {path}: {error}") from error
    return {
        "path": relative,
        "sha256": _sha256(path),
        "size_bytes": size_bytes,
    }


def _validate_artifact_reference(
    value: Any,
    *,
    baseline_path: pathlib.Path,
    label: str,
    maximum_size: int,
) -> pathlib.Path:
    _require_type(value, dict, label)
    _require_exact_keys(value, _ARTIFACT_KEYS, label)
    path = _safe_artifact_path(baseline_path, value["path"], f"{label}.path")
    expected_sha256 = _require_sha256(value["sha256"], f"{label}.sha256")
    expected_size = _require_integer(
        value["size_bytes"],
        f"{label}.size_bytes",
    )
    if expected_size > maximum_size:
        raise HarnessError(f"{label} exceeds its size limit")
    try:
        actual_size = path.stat().st_size
    except OSError as error:
        raise HarnessError(f"cannot inspect artifact {path}: {error}") from error
    if actual_size != expected_size:
        raise HarnessError(f"{label} artifact size does not match")
    if _sha256(path) != expected_sha256:
        raise HarnessError(f"{label} artifact SHA-256 does not match")
    return path


def _command_output(
    command: list[str],
    *,
    cwd: pathlib.Path,
    label: str,
    stdout_limit: int = 16 * 1024 * 1024,
) -> bytes:
    result = run_bounded(
        command,
        cwd=cwd,
        timeout_seconds=30.0,
        stdout_limit=stdout_limit,
        stderr_limit=1024 * 1024,
    )
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise HarnessError(f"{label} failed with exit {result.returncode}: {message}")
    return result.stdout


def _git_output(
    repository_root: pathlib.Path,
    arguments: list[str],
    label: str,
) -> bytes:
    return _command_output(
        ["git", *arguments],
        cwd=repository_root,
        label=label,
    )


def _filtered_git_status(
    repository_root: pathlib.Path,
    excluded_path: str,
) -> bytes:
    status = _git_output(
        repository_root,
        ["status", "--porcelain=v1", "-z", "--untracked-files=all"],
        "git status",
    )
    if not excluded_path:
        return status
    retained = []
    excluded_prefix = f"{excluded_path}/"
    for entry in status.split(b"\0"):
        if not entry:
            continue
        if len(entry) < 4:
            raise HarnessError("git status returned a malformed entry")
        path = entry[3:].decode("utf-8", errors="strict")
        if path == excluded_path or path.startswith(excluded_prefix):
            continue
        retained.append(entry)
    return b"".join(entry + b"\0" for entry in retained)


def _worktree_content_sha256(
    repository_root: pathlib.Path,
    excluded_path: str,
) -> str:
    listed = _git_output(
        repository_root,
        ["ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        "git ls-files",
    )
    excluded_prefix = f"{excluded_path}/" if excluded_path else ""
    paths = []
    for raw_path in listed.split(b"\0"):
        if not raw_path:
            continue
        relative = raw_path.decode("utf-8", errors="strict")
        if excluded_path and (
            relative == excluded_path or relative.startswith(excluded_prefix)
        ):
            continue
        paths.append(relative)
    paths.sort()

    digest = hashlib.sha256()
    for relative in paths:
        path = repository_root / pathlib.PurePosixPath(relative)
        try:
            metadata = path.lstat()
            if path.is_symlink():
                kind = b"symlink"
                content = os.readlink(path).encode("utf-8")
            elif path.is_file():
                kind = b"file"
                content = path.read_bytes()
            else:
                raise HarnessError(
                    f"worktree path is not a file or symlink: {relative}"
                )
        except OSError as error:
            raise HarnessError(
                f"cannot hash worktree path {relative}: {error}"
            ) from error
        path_bytes = relative.encode("utf-8")
        executable = b"1" if metadata.st_mode & 0o111 else b"0"
        for value in (path_bytes, kind, executable, content):
            digest.update(len(value).to_bytes(8, "big"))
            digest.update(value)
    return digest.hexdigest()


def _require_git_oid(value: Any, label: str) -> str:
    text = _require_string(value, label)
    if re.fullmatch(r"[0-9a-f]{40,64}", text) is None:
        raise HarnessError(f"{label} must be a lowercase Git object ID")
    return text


def _capture_source_state(
    repository_root: pathlib.Path,
    excluded_path: str,
) -> dict[str, Any]:
    top_level = _git_output(
        repository_root,
        ["rev-parse", "--show-toplevel"],
        "git repository lookup",
    ).decode("utf-8", errors="strict").strip()
    if pathlib.Path(top_level).resolve() != repository_root.resolve():
        raise HarnessError("repository root does not match git rev-parse")
    revision = _git_output(
        repository_root,
        ["rev-parse", "HEAD"],
        "git revision lookup",
    ).decode("ascii", errors="strict").strip()
    tree = _git_output(
        repository_root,
        ["rev-parse", "HEAD^{tree}"],
        "git tree lookup",
    ).decode("ascii", errors="strict").strip()
    _require_git_oid(revision, "source revision")
    _require_git_oid(tree, "source tree")
    status = _filtered_git_status(repository_root, excluded_path)
    return {
        "revision": revision,
        "tree": tree,
        "clean": not status,
        "status_sha256": hashlib.sha256(status).hexdigest(),
        "worktree_content_sha256": _worktree_content_sha256(
            repository_root,
            excluded_path,
        ),
    }


def _parse_cmake_cache(cache_path: pathlib.Path) -> dict[str, str]:
    try:
        lines = cache_path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise HarnessError(f"cannot read {cache_path}: {error}") from error
    values = {}
    for line in lines:
        if not line or line.startswith(("//", "#")) or "=" not in line:
            continue
        name_and_type, value = line.split("=", 1)
        name = name_and_type.split(":", 1)[0]
        values[name] = value
    return values


def _find_cmake_cache(binary_path: pathlib.Path) -> pathlib.Path:
    for parent in (binary_path.parent, *binary_path.parents):
        candidate = parent / "CMakeCache.txt"
        if candidate.is_file():
            return candidate
    raise HarnessError(f"cannot find CMakeCache.txt for {binary_path}")


def _version_line(executable: str, repository_root: pathlib.Path) -> str:
    output = _command_output(
        [executable, "--version"],
        cwd=repository_root,
        label=f"{executable} version",
    ).decode("utf-8", errors="strict")
    first_line = output.splitlines()
    if not first_line or not first_line[0]:
        raise HarnessError(f"{executable} did not report a version")
    return first_line[0]


def _effective_flags(base_flags: str, release_flags: str) -> str:
    return " ".join(
        value.strip() for value in (base_flags, release_flags) if value.strip()
    )


def _validate_benchmark_flags(
    flags: str,
    *,
    require_optimization: bool,
    label: str,
) -> None:
    try:
        tokens = shlex.split(flags, posix=os.name != "nt")
    except ValueError as error:
        raise HarnessError(f"{label} cannot be parsed: {error}") from error
    forbidden_exact = {
        "--coverage",
        "-O0",
        "-Og",
        "-p",
        "-pg",
    }
    forbidden_prefixes = (
        "-finstrument-functions",
        "-flto",
        "-fsanitize=",
        "-fsanitize-coverage",
        "-fprofile",
        "-fcoverage",
        "-ftest-coverage",
        "-fxray-instrument",
    )
    for token in tokens:
        normalized = token.lower()
        if (
            token in forbidden_exact
            or token.startswith(forbidden_prefixes)
            or normalized in {"/od", "/profile", "-wl,-p", "-wl,-pg"}
            or normalized.startswith(
                ("/fsanitize", "/gl", "/ltcg", "/gh")
            )
            or "xray" in normalized
        ):
            raise HarnessError(
                f"{label} contains forbidden benchmark flag: {token}"
            )
    if require_optimization and not any(
        re.fullmatch(r"-O(?:[123sSz]|fast)?", token)
        or token.lower() in {"/o1", "/o2", "/ox"}
        for token in tokens
    ):
        raise HarnessError(
            f"{label} must contain an actual optimized compiler flag"
        )


def _normalize_actual_flag(
    value: str,
    *,
    repository_root: pathlib.Path,
    build_directory: pathlib.Path,
    sqlite_source_directory: pathlib.Path,
) -> str:
    replacements = sorted(
        (
            (str(build_directory), "<BUILD_DIRECTORY>"),
            (str(sqlite_source_directory), "<SQLITE_SOURCE_DIRECTORY>"),
            (str(repository_root), "<REPOSITORY_ROOT>"),
            (str(pathlib.Path.home()), "<HOME>"),
        ),
        key=lambda item: len(item[0]),
        reverse=True,
    )
    result = value
    for original, replacement in replacements:
        result = result.replace(original, replacement)
    return result


def _extract_compile_flags(
    command: str,
    *,
    repository_root: pathlib.Path,
    build_directory: pathlib.Path,
    sqlite_source_directory: pathlib.Path,
) -> str:
    try:
        tokens = shlex.split(command, posix=os.name != "nt")
    except ValueError as error:
        raise HarnessError(f"compile command cannot be parsed: {error}") from error
    if len(tokens) < 2:
        raise HarnessError("compile command is incomplete")
    retained = []
    index = 1
    options_with_values = {"-MF", "-MT", "-o", "-c", "/Fo"}
    standalone_generated_options = {"-MD", "-MMD"}
    while index < len(tokens):
        token = tokens[index]
        if token in options_with_values:
            index += 2
            continue
        if token in standalone_generated_options:
            index += 1
            continue
        if token.startswith("/Fo"):
            index += 1
            continue
        retained.append(
            _normalize_actual_flag(
                token,
                repository_root=repository_root,
                build_directory=build_directory,
                sqlite_source_directory=sqlite_source_directory,
            )
        )
        index += 1
    if not retained:
        raise HarnessError("compile command has no effective flags")
    return shlex.join(retained)


def _extract_link_flags(
    command: str,
    *,
    compiler_path: str,
    output_name: str,
    repository_root: pathlib.Path,
    build_directory: pathlib.Path,
    sqlite_source_directory: pathlib.Path,
) -> str:
    try:
        tokens = shlex.split(command, posix=os.name != "nt")
    except ValueError as error:
        raise HarnessError(f"link command cannot be parsed: {error}") from error
    compiler_name = pathlib.Path(compiler_path).name
    compiler_index = next(
        (
            index
            for index, token in enumerate(tokens)
            if pathlib.Path(token).name == compiler_name
        ),
        None,
    )
    if compiler_index is None:
        raise HarnessError("link command does not invoke the configured compiler")
    retained = []
    index = compiler_index + 1
    while index < len(tokens) and tokens[index] != "&&":
        token = tokens[index]
        if token == "-o":
            index += 2
            continue
        normalized = token.lower()
        if normalized.startswith("/out:"):
            index += 1
            continue
        if (
            token == output_name
            or token.endswith((".a", ".dylib", ".lib", ".o", ".obj", ".so"))
        ):
            index += 1
            continue
        retained.append(
            _normalize_actual_flag(
                token,
                repository_root=repository_root,
                build_directory=build_directory,
                sqlite_source_directory=sqlite_source_directory,
            )
        )
        index += 1
    if not retained:
        raise HarnessError("link command has no effective flags")
    return shlex.join(retained)


def _capture_actual_build_flags(
    *,
    cache: dict[str, str],
    repository_root: pathlib.Path,
    build_directory: pathlib.Path,
    sqlite_source_directory: pathlib.Path,
) -> dict[str, str]:
    if cache["CMAKE_GENERATOR"] != "Ninja":
        raise HarnessError("benchmark provenance requires the pinned Ninja generator")
    compile_commands_path = build_directory / "compile_commands.json"
    compile_commands = load_json_strict(compile_commands_path)
    _require_type(
        compile_commands,
        list,
        "benchmark compile_commands.json",
    )
    if not compile_commands:
        raise HarnessError("benchmark compile_commands.json must be nonempty")

    targets = {
        "actual_sqlite_c_compile_flags": (
            "CMakeFiles/modern_sqlite_benchmark_sqlite.dir/",
            True,
        ),
        "actual_modern_cxx_compile_flags": (
            "CMakeFiles/modern_sqlite.dir/",
            True,
        ),
        "actual_harness_cxx_compile_flags": (
            "CMakeFiles/modern_sqlite_read_benchmark.dir/",
            True,
        ),
    }
    captured: dict[str, str] = {}
    for field, (needle, require_optimization) in targets.items():
        flags = set()
        for index, entry in enumerate(compile_commands):
            _require_type(
                entry,
                dict,
                f"benchmark compile command {index}",
            )
            command = _require_string(
                entry.get("command"),
                f"benchmark compile command {index}.command",
            )
            if needle not in command:
                continue
            extracted = _extract_compile_flags(
                command,
                repository_root=repository_root,
                build_directory=build_directory,
                sqlite_source_directory=sqlite_source_directory,
            )
            _validate_benchmark_flags(
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
    commands = _command_output(
        [
            make_program,
            "-C",
            str(build_directory),
            "-t",
            "commands",
            "modern_sqlite_read_benchmark",
        ],
        cwd=repository_root,
        label="Ninja timing-target command lookup",
    ).decode("utf-8", errors="strict")
    link_candidates = [
        line
        for line in commands.splitlines()
        if "modern_sqlite_read_benchmark" in line
        and " -c " not in line
        and (
            " -o modern_sqlite_read_benchmark " in line
            or "/OUT:MODERN_SQLITE_READ_BENCHMARK" in line.upper()
        )
    ]
    if len(link_candidates) != 1:
        raise HarnessError(
            "Ninja command graph must contain one timing executable link command"
        )
    link_flags = _extract_link_flags(
        link_candidates[0],
        compiler_path=cache["CMAKE_CXX_COMPILER"],
        output_name="modern_sqlite_read_benchmark",
        repository_root=repository_root,
        build_directory=build_directory,
        sqlite_source_directory=sqlite_source_directory,
    )
    _validate_benchmark_flags(
        link_flags,
        require_optimization=False,
        label="actual_timing_link_flags",
    )
    captured["actual_timing_link_flags"] = link_flags
    return captured


def _capture_build_provenance(
    *,
    repository_root: pathlib.Path,
    timing_binary_path: pathlib.Path,
    diagnostic_binary_path: pathlib.Path,
    sqlite_source_directory: pathlib.Path,
) -> dict[str, Any]:
    timing_cache_path = _find_cmake_cache(timing_binary_path)
    diagnostic_cache_path = _find_cmake_cache(diagnostic_binary_path)
    if timing_cache_path.resolve() != diagnostic_cache_path.resolve():
        raise HarnessError("benchmark binaries must share one CMake build directory")
    cache = _parse_cmake_cache(timing_cache_path)
    diagnostic_cache = _parse_cmake_cache(diagnostic_cache_path)
    required = (
        "CMAKE_BUILD_TYPE",
        "CMAKE_C_COMPILER",
        "CMAKE_CXX_COMPILER",
        "CMAKE_GENERATOR",
        "CMAKE_HOME_DIRECTORY",
        "CMAKE_MAKE_PROGRAM",
    )
    missing = [
        name
        for name in required
        if not cache.get(name) or not diagnostic_cache.get(name)
    ]
    if missing:
        raise HarnessError(f"CMake cache is missing required values: {missing}")
    if cache["CMAKE_BUILD_TYPE"] != "Release":
        raise HarnessError("benchmark CMake cache must be a Release build")
    if pathlib.Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != (
        repository_root.resolve()
    ) or pathlib.Path(diagnostic_cache["CMAKE_HOME_DIRECTORY"]).resolve() != (
        repository_root.resolve()
    ):
        raise HarnessError(
            "benchmark CMake source directory does not match the repository root"
        )
    compared_cache_keys = required + (
        "CMAKE_C_FLAGS",
        "CMAKE_C_FLAGS_RELEASE",
        "CMAKE_CXX_FLAGS",
        "CMAKE_CXX_FLAGS_RELEASE",
        "CMAKE_EXE_LINKER_FLAGS",
        "CMAKE_EXE_LINKER_FLAGS_RELEASE",
    )
    if any(
        cache.get(key, "") != diagnostic_cache.get(key, "")
        for key in compared_cache_keys
    ):
        raise HarnessError("benchmark binaries use different CMake build inputs")
    c_base_flags = cache.get("CMAKE_C_FLAGS", "")
    c_release_flags = cache.get("CMAKE_C_FLAGS_RELEASE", "")
    c_effective_flags = _effective_flags(c_base_flags, c_release_flags)
    cxx_base_flags = cache.get("CMAKE_CXX_FLAGS", "")
    cxx_release_flags = cache.get("CMAKE_CXX_FLAGS_RELEASE", "")
    cxx_effective_flags = _effective_flags(
        cxx_base_flags,
        cxx_release_flags,
    )
    linker_base_flags = cache.get("CMAKE_EXE_LINKER_FLAGS", "")
    linker_release_flags = cache.get(
        "CMAKE_EXE_LINKER_FLAGS_RELEASE",
        "",
    )
    linker_effective_flags = _effective_flags(
        linker_base_flags,
        linker_release_flags,
    )
    _validate_benchmark_flags(
        c_effective_flags,
        require_optimization=True,
        label="effective C benchmark flags",
    )
    _validate_benchmark_flags(
        cxx_effective_flags,
        require_optimization=True,
        label="effective C++ benchmark flags",
    )
    _validate_benchmark_flags(
        linker_effective_flags,
        require_optimization=False,
        label="effective benchmark linker flags",
    )
    actual_build_flags = _capture_actual_build_flags(
        cache=cache,
        repository_root=repository_root,
        build_directory=timing_cache_path.parent,
        sqlite_source_directory=sqlite_source_directory,
    )
    cmake = shutil.which("cmake")
    if cmake is None:
        raise HarnessError("cmake is required to record benchmark provenance")
    return {
        "cmake_version": _version_line(cmake, repository_root),
        "generator": cache["CMAKE_GENERATOR"],
        "build_type": cache["CMAKE_BUILD_TYPE"],
        "c_compiler": cache["CMAKE_C_COMPILER"],
        "c_compiler_version": _version_line(
            cache["CMAKE_C_COMPILER"],
            repository_root,
        ),
        "c_base_flags": c_base_flags,
        "c_release_flags": c_release_flags,
        "c_effective_flags": c_effective_flags,
        "cxx_compiler": cache["CMAKE_CXX_COMPILER"],
        "cxx_compiler_version": _version_line(
            cache["CMAKE_CXX_COMPILER"],
            repository_root,
        ),
        "cxx_base_flags": cxx_base_flags,
        "cxx_release_flags": cxx_release_flags,
        "cxx_effective_flags": cxx_effective_flags,
        "linker_base_flags": linker_base_flags,
        "linker_release_flags": linker_release_flags,
        "linker_effective_flags": linker_effective_flags,
        **actual_build_flags,
    }


def _cpu_identity(repository_root: pathlib.Path) -> str:
    system = platform.system()
    if system == "Darwin":
        sysctl = shutil.which("sysctl")
        if sysctl is not None:
            for key in ("machdep.cpu.brand_string", "hw.model"):
                result = run_bounded(
                    [sysctl, "-n", key],
                    cwd=repository_root,
                    timeout_seconds=5.0,
                    stdout_limit=64 * 1024,
                    stderr_limit=64 * 1024,
                )
                if result.returncode == 0:
                    value = result.stdout.decode(
                        "utf-8",
                        errors="strict",
                    ).strip()
                    if value:
                        return value
    elif system == "Linux":
        cpuinfo = pathlib.Path("/proc/cpuinfo")
        try:
            lines = cpuinfo.read_text(encoding="utf-8").splitlines()
        except OSError:
            lines = []
        for field in ("model name", "hardware", "processor"):
            prefix = f"{field}\t:"
            for line in lines:
                normalized = line.lower()
                if normalized.startswith(prefix):
                    value = line.split(":", 1)[1].strip()
                    if value:
                        return value
    return platform.processor() or platform.machine() or "unknown"


def _capture_host_provenance(
    repository_root: pathlib.Path,
) -> dict[str, Any]:
    return {
        "os": platform.system(),
        "kernel": platform.release(),
        "architecture": platform.machine(),
        "cpu": _cpu_identity(repository_root),
        "logical_cpu_count": os.cpu_count() or 1,
        "wall_timer": "steady_clock",
        "cpu_timer": "CLOCK_PROCESS_CPUTIME_ID",
    }


def _validate_identity_report(
    value: Any,
    *,
    expected_source: dict[str, Any],
    expected_semantic_options: list[str],
    instrumentation: bool,
    label: str,
) -> dict[str, Any]:
    _require_type(value, dict, label)
    _require_exact_keys(value, _IDENTITY_KEYS, label)
    if value["schema_version"] != SCHEMA_VERSION or value["mode"] != "identity":
        raise HarnessError(f"{label} schema or mode is invalid")
    build = value["build"]
    _require_type(build, dict, f"{label}.build")
    _require_exact_keys(build, _IDENTITY_BUILD_KEYS, f"{label}.build")
    if build["build_type"] != "Release":
        raise HarnessError(f"{label}.build must be Release")
    if build["instrumentation"] is not instrumentation:
        raise HarnessError(f"{label}.build instrumentation is invalid")
    if build["sanitizers"] is not False or build["coverage"] is not False:
        raise HarnessError(f"{label}.build cannot use sanitizers or coverage")
    _require_integer(build["cplusplus"], f"{label}.build.cplusplus", minimum=202100)
    _require_string(build["architecture"], f"{label}.build.architecture")
    for key, allowed in (
        ("compiler", {"clang", "gcc", "msvc"}),
        ("standard_library", {"libc++", "libstdc++", "msvc-stl"}),
    ):
        named = build[key]
        _require_type(named, dict, f"{label}.build.{key}")
        _require_exact_keys(
            named,
            _IDENTITY_NAMED_VERSION_KEYS,
            f"{label}.build.{key}",
        )
        if named["id"] not in allowed:
            raise HarnessError(f"{label}.build.{key}.id is unsupported")
        _require_string(named["version"], f"{label}.build.{key}.version")
    source = value["source"]
    _require_type(source, dict, f"{label}.source")
    _require_exact_keys(source, _IDENTITY_SOURCE_KEYS, f"{label}.source")
    _require_git_oid(source["revision"], f"{label}.source.revision")
    _require_git_oid(source["tree"], f"{label}.source.tree")
    if source != {
        "revision": expected_source["revision"],
        "tree": expected_source["tree"],
    }:
        raise HarnessError(
            f"{label}.source does not match the current Git revision and tree"
        )
    _validate_sqlite(
        value["sqlite"],
        expected_semantic_options=expected_semantic_options,
        label=f"{label}.sqlite",
    )
    return value


def _read_binary_identity(
    *,
    binary_path: pathlib.Path,
    repository_root: pathlib.Path,
    expected_source: dict[str, Any],
    expected_semantic_options: list[str],
    instrumentation: bool,
    child_runner: Any,
) -> dict[str, Any]:
    result = child_runner(
        [str(binary_path), "identity"],
        cwd=repository_root,
        timeout_seconds=30.0,
        stdout_limit=1024 * 1024,
        stderr_limit=1024 * 1024,
    )
    if result.returncode != 0 or result.stderr:
        raise HarnessError(f"benchmark identity command failed for {binary_path}")
    return _validate_identity_report(
        load_json_bytes_strict(result.stdout, f"{binary_path.name} identity"),
        expected_source=expected_source,
        expected_semantic_options=expected_semantic_options,
        instrumentation=instrumentation,
        label=f"{binary_path.name} identity",
    )


def _repository_input_reference(
    *,
    role: str,
    path: pathlib.Path,
    repository_root: pathlib.Path,
) -> dict[str, Any]:
    if not path.is_file():
        raise HarnessError(f"required input does not exist: {path}")
    return {
        "role": role,
        "path": _repository_relative_path(path, repository_root, role),
        "sha256": _sha256(path),
        "size_bytes": path.stat().st_size,
    }


def _collect_input_references(
    *,
    repository_root: pathlib.Path,
    workload_path: pathlib.Path,
    workload_manifest: dict[str, Any],
) -> list[dict[str, Any]]:
    profile_path = _resolve_repository_file(
        repository_root,
        workload_manifest["sqlite_profile"],
        "workload manifest.sqlite_profile",
    )
    inputs = [
        _repository_input_reference(
            role="workload-manifest",
            path=workload_path,
            repository_root=repository_root,
        ),
        _repository_input_reference(
            role="sqlite-profile",
            path=profile_path,
            repository_root=repository_root,
        ),
        _repository_input_reference(
            role="benchmark-source",
            path=repository_root / "benchmarks/read_performance.cpp",
            repository_root=repository_root,
        ),
        _repository_input_reference(
            role="runner-source",
            path=repository_root / "tools/read_performance.py",
            repository_root=repository_root,
        ),
    ]
    for fixture in workload_manifest["fixtures"]:
        inputs.append(
            _repository_input_reference(
                role=f"fixture-{fixture['id']}",
                path=repository_root / pathlib.PurePosixPath(fixture["path"]),
                repository_root=repository_root,
            )
        )
        inputs.append(
            _repository_input_reference(
                role=f"fixture-sql-{fixture['id']}",
                path=repository_root / pathlib.PurePosixPath(fixture["sql_path"]),
                repository_root=repository_root,
            )
        )
    return inputs


def _binary_reference(
    *,
    path: pathlib.Path,
    identity: dict[str, Any],
) -> dict[str, Any]:
    if not path.is_file():
        raise HarnessError(f"benchmark binary does not exist: {path}")
    return {
        "logical_name": path.name,
        "sha256": _sha256(path),
        "size_bytes": path.stat().st_size,
        "identity": identity,
    }


def _sidecars(path: pathlib.Path) -> list[pathlib.Path]:
    return [
        pathlib.Path(f"{path}-journal"),
        pathlib.Path(f"{path}-wal"),
        pathlib.Path(f"{path}-shm"),
    ]


def _run_one_baseline_child(
    *,
    baseline_path: pathlib.Path,
    repository_root: pathlib.Path,
    binary_path: pathlib.Path,
    binary_logical_name: str,
    workload_manifest: dict[str, Any],
    case: dict[str, Any],
    engine: str,
    run_kind: str,
    ordinal: int,
    round_index: int | None,
    child_runner: Any,
) -> tuple[dict[str, Any], dict[str, Any]]:
    fixture = next(
        item
        for item in workload_manifest["fixtures"]
        if item["id"] == case["fixture"]
    )
    fixture_path = repository_root / pathlib.PurePosixPath(fixture["path"])
    stem = (
        f"{ordinal:03d}-round-{round_index}-{case['id']}-{engine}"
        if round_index is not None
        else f"{ordinal:03d}-{case['id']}-{engine}"
    )
    scratch_path = baseline_path / "scratch" / f"{stem}.db"
    raw_group = "timing" if run_kind == "baseline" else "diagnostic"
    stdout_path = baseline_path / "raw" / raw_group / f"{stem}.json"
    stderr_path = baseline_path / "raw" / raw_group / f"{stem}.stderr"
    scratch_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copyfile(fixture_path, scratch_path)
    except OSError as error:
        raise HarnessError(f"cannot copy benchmark fixture: {error}") from error
    before_sha256 = _sha256(scratch_path)
    if before_sha256 != fixture["sha256"]:
        raise HarnessError("fresh benchmark database copy has the wrong SHA-256")

    command = [
        str(binary_path),
        "run",
        engine,
        case["id"],
        str(scratch_path),
        run_kind,
    ]
    execution_error = None
    try:
        result = child_runner(
            command,
            cwd=repository_root,
            timeout_seconds=600.0 if run_kind == "baseline" else 300.0,
            stdout_limit=4 * 1024 * 1024,
            stderr_limit=1024 * 1024,
        )
        timed_out = False
    except ChildExecutionError as error:
        execution_error = error
        timed_out = error.timed_out
        result = ChildResult(
            returncode=error.returncode,
            stdout=error.stdout,
            stderr=error.stderr,
            elapsed_ns=error.elapsed_ns,
        )
    _write_new_bytes(stdout_path, result.stdout)
    _write_new_bytes(stderr_path, result.stderr)
    after_sha256 = _sha256(scratch_path)
    remaining_sidecars = [
        path.name for path in _sidecars(scratch_path) if path.exists()
    ]
    database = {
        "fixture": fixture["id"],
        "source_path": fixture["path"],
        "copy_path": scratch_path.relative_to(baseline_path).as_posix(),
        "before_sha256": before_sha256,
        "after_sha256": after_sha256,
        "sidecars_remaining": remaining_sidecars,
        "cleanup": "preserved",
    }
    process = {
        "returncode": result.returncode,
        "timed_out": timed_out,
        "runner_elapsed_ns": result.elapsed_ns,
    }
    record = {
        "ordinal": ordinal,
        "case": case["id"],
        "engine": engine,
        "logical_argv": [
            binary_logical_name,
            "run",
            engine,
            case["id"],
            database["copy_path"],
            run_kind,
        ],
        "database": database,
        "process": process,
        "stdout": _artifact_reference(stdout_path, baseline_path),
        "stderr": _artifact_reference(stderr_path, baseline_path),
    }
    if round_index is not None:
        record["round"] = round_index

    if execution_error is not None:
        raise BaselineChildFailure(str(execution_error), record) from execution_error
    if result.returncode == 2:
        raise BaselineChildMismatch(
            f"{run_kind} child reported a correctness mismatch for "
            f"{engine} {case['id']}",
            record,
        )
    if result.returncode != 0:
        raise BaselineChildFailure(
            f"{run_kind} child failed for {engine} {case['id']} "
            f"with exit {result.returncode}",
            record,
        )
    if result.stderr:
        raise BaselineChildFailure(
            f"{run_kind} child wrote stderr for {engine} {case['id']}",
            record,
        )
    if after_sha256 != before_sha256 or remaining_sidecars:
        raise BaselineChildMismatch(
            f"{run_kind} child changed its database for {engine} {case['id']}",
            record,
        )

    try:
        report = load_json_bytes_strict(
            result.stdout,
            f"{run_kind} {engine} {case['id']}",
        )
        if run_kind == "baseline":
            validate_raw_timing_report(
                report,
                workload_manifest=workload_manifest,
                expected_engine=engine,
                expected_case=case["id"],
                expected_run_kind="baseline",
            )
        else:
            validate_raw_diagnostic_report(
                report,
                workload_manifest=workload_manifest,
                expected_engine=engine,
                expected_case=case["id"],
            )
    except HarnessError as error:
        raise BaselineChildFailure(str(error), record) from error
    try:
        scratch_path.unlink()
        (baseline_path / "scratch").rmdir()
    except OSError as error:
        raise BaselineChildFailure(
            f"cannot remove owned database copy: {error}",
            record,
        ) from error
    database["cleanup"] = "removed"
    return record, report


def _failure_report(error: BaseException) -> dict[str, Any]:
    report = {
        "schema_version": SCHEMA_VERSION,
        "status": "failed",
        "error_kind": type(error).__name__,
        "message": str(error),
    }
    record = getattr(error, "record", None)
    if record is not None:
        report["child"] = record
    return report


def generate_baseline(
    *,
    repository_root: pathlib.Path,
    workload_path: pathlib.Path,
    timing_binary_path: pathlib.Path,
    diagnostic_binary_path: pathlib.Path,
    sqlite_c_path: pathlib.Path,
    sqlite_h_path: pathlib.Path,
    output_path: pathlib.Path,
    child_runner: Any = run_bounded,
) -> dict[str, Any]:
    repository_root = repository_root.resolve()
    workload_path = workload_path.resolve()
    timing_binary_path = timing_binary_path.resolve()
    diagnostic_binary_path = diagnostic_binary_path.resolve()
    sqlite_c_path = sqlite_c_path.resolve()
    sqlite_h_path = sqlite_h_path.resolve()
    output_path = output_path.resolve()
    output_relative = _repository_relative_path(
        output_path,
        repository_root,
        "baseline output",
    )
    validate_new_output_path(
        output_path,
        protected_paths=[
            workload_path,
            timing_binary_path,
            diagnostic_binary_path,
            sqlite_c_path,
            sqlite_h_path,
        ],
    )
    if paths_refer_to_same_file(timing_binary_path, diagnostic_binary_path):
        raise HarnessError("timing and diagnostic binaries must be distinct")

    workload_manifest = validate_workload_manifest(
        load_json_strict(workload_path),
        repository_root=repository_root,
        manifest_path=workload_path,
    )
    profile_path = _resolve_repository_file(
        repository_root,
        workload_manifest["sqlite_profile"],
        "workload manifest.sqlite_profile",
    )
    profile = _validate_profile(
        load_json_strict(profile_path),
        label="SQLite profile",
    )
    sqlite_profile = profile["sqlite"]
    sqlite_c_sha256 = _sha256(sqlite_c_path)
    sqlite_h_sha256 = _sha256(sqlite_h_path)
    if sqlite_c_sha256 != sqlite_profile["sqlite3_c_sha256"]:
        raise HarnessError("pinned sqlite3.c SHA-256 does not match the profile")
    if sqlite_h_sha256 != sqlite_profile["sqlite3_h_sha256"]:
        raise HarnessError("pinned sqlite3.h SHA-256 does not match the profile")

    source = _capture_source_state(repository_root, output_relative)
    if not source["clean"]:
        raise HarnessError("baseline generation requires a clean source worktree")
    timing_identity = _read_binary_identity(
        binary_path=timing_binary_path,
        repository_root=repository_root,
        expected_source=source,
        expected_semantic_options=workload_manifest[
            "sqlite_semantic_compile_options"
        ],
        instrumentation=False,
        child_runner=child_runner,
    )
    diagnostic_identity = _read_binary_identity(
        binary_path=diagnostic_binary_path,
        repository_root=repository_root,
        expected_source=source,
        expected_semantic_options=workload_manifest[
            "sqlite_semantic_compile_options"
        ],
        instrumentation=True,
        child_runner=child_runner,
    )
    if timing_identity["sqlite"] != diagnostic_identity["sqlite"]:
        raise HarnessError("benchmark binaries report different SQLite identities")
    timing_build_identity = dict(timing_identity["build"])
    diagnostic_build_identity = dict(diagnostic_identity["build"])
    timing_build_identity.pop("instrumentation")
    diagnostic_build_identity.pop("instrumentation")
    if timing_build_identity != diagnostic_build_identity:
        raise HarnessError("benchmark binaries report unmatched build identities")
    if _sha256(timing_binary_path) == _sha256(diagnostic_binary_path):
        raise HarnessError("benchmark binary contents must be distinct")
    build_provenance = _capture_build_provenance(
        repository_root=repository_root,
        timing_binary_path=timing_binary_path,
        diagnostic_binary_path=diagnostic_binary_path,
        sqlite_source_directory=sqlite_c_path.parent,
    )
    binary_provenance = {
        "timing": _binary_reference(
            path=timing_binary_path,
            identity=timing_identity,
        ),
        "diagnostic": _binary_reference(
            path=diagnostic_binary_path,
            identity=diagnostic_identity,
        ),
    }

    try:
        output_path.mkdir(parents=True)
    except OSError as error:
        raise HarnessError(f"cannot create baseline output: {error}") from error

    timing_runs = []
    diagnostic_runs = []
    timing_reports: dict[tuple[int, str, str], dict[str, Any]] = {}
    try:
        case_by_id = {case["id"]: case for case in workload_manifest["cases"]}
        ordinal = 0
        for round_definition in workload_manifest["rounds"]:
            ordered_case_ids = list(EXPECTED_CASE_IDS)
            if round_definition["case_order"] == "reverse":
                ordered_case_ids.reverse()
            for case_id in ordered_case_ids:
                case = case_by_id[case_id]
                for engine in round_definition["engine_order"]:
                    record, report = _run_one_baseline_child(
                        baseline_path=output_path,
                        repository_root=repository_root,
                        binary_path=timing_binary_path,
                        binary_logical_name=timing_binary_path.name,
                        workload_manifest=workload_manifest,
                        case=case,
                        engine=engine,
                        run_kind="baseline",
                        ordinal=ordinal,
                        round_index=round_definition["index"],
                        child_runner=child_runner,
                    )
                    timing_runs.append(record)
                    timing_reports[
                        (round_definition["index"], case_id, engine)
                    ] = report
                    ordinal += 1

        ordinal = 0
        for case in workload_manifest["cases"]:
            for engine in ("modern", "sqlite"):
                record, _ = _run_one_baseline_child(
                    baseline_path=output_path,
                    repository_root=repository_root,
                    binary_path=diagnostic_binary_path,
                    binary_logical_name=diagnostic_binary_path.name,
                    workload_manifest=workload_manifest,
                    case=case,
                    engine=engine,
                    run_kind="diagnostic",
                    ordinal=ordinal,
                    round_index=None,
                    child_runner=child_runner,
                )
                diagnostic_runs.append(record)
                ordinal += 1

        for name, path in (
            ("timing", timing_binary_path),
            ("diagnostic", diagnostic_binary_path),
        ):
            try:
                current_size = path.stat().st_size
            except OSError as error:
                raise HarnessError(
                    f"cannot recheck {name} benchmark binary: {error}"
                ) from error
            if (
                _sha256(path) != binary_provenance[name]["sha256"]
                or current_size != binary_provenance[name]["size_bytes"]
            ):
                raise HarnessError(
                    f"{name} benchmark binary changed during baseline generation"
                )
        if (
            _sha256(sqlite_c_path) != sqlite_c_sha256
            or _sha256(sqlite_h_path) != sqlite_h_sha256
        ):
            raise HarnessError(
                "pinned SQLite source inputs changed during baseline generation"
            )
        run_manifest = {
            "schema_version": SCHEMA_VERSION,
            "workload_semantics_version": WORKLOAD_SEMANTICS_VERSION,
            "completion_schema_version": COMPLETION_SCHEMA_VERSION,
            "diagnostic_schema_version": DIAGNOSTIC_SCHEMA_VERSION,
            "status": "complete",
            "workload_manifest": {
                "path": _repository_relative_path(
                    workload_path,
                    repository_root,
                    "workload manifest",
                ),
                "sha256": _sha256(workload_path),
                "size_bytes": workload_path.stat().st_size,
            },
            "source": source,
            "sqlite": {
                "version": SQLITE_VERSION,
                "source_id": SQLITE_SOURCE_ID,
                "compile_options": profile["build"]["semantic_compile_options"],
                "sqlite3_c_sha256": sqlite_profile["sqlite3_c_sha256"],
                "sqlite3_h_sha256": sqlite_profile["sqlite3_h_sha256"],
                "library_role": "statically-linked-benchmark-oracle",
            },
            "build": build_provenance,
            "host": _capture_host_provenance(repository_root),
            "inputs": _collect_input_references(
                repository_root=repository_root,
                workload_path=workload_path,
                workload_manifest=workload_manifest,
            ),
            "binaries": binary_provenance,
            "schedule": workload_manifest["rounds"],
            "timing_runs": timing_runs,
            "diagnostic_runs": diagnostic_runs,
        }
        run_manifest_path = output_path / "run-manifest.json"
        _write_new_json(run_manifest_path, run_manifest)
        aggregate = build_aggregate_report(
            workload_manifest=workload_manifest,
            timing_reports=timing_reports,
            run_manifest_path="run-manifest.json",
            run_manifest_sha256=_sha256(run_manifest_path),
        )
        _write_new_json(output_path / "aggregate.json", aggregate)
        validate_baseline_directory(
            baseline_path=output_path,
            repository_root=repository_root,
            workload_path=workload_path,
            verify_current_source=True,
        )
        enforce_performance_guard(aggregate)
        return aggregate
    except (HarnessError, PerformanceMismatch) as error:
        failure_path = output_path / "failure.json"
        if not failure_path.exists():
            _write_new_json(failure_path, _failure_report(error))
        raise


def _validate_source_state(
    value: Any,
    *,
    repository_root: pathlib.Path,
    baseline_relative: str,
    verify_current: bool,
) -> None:
    _require_type(value, dict, "run manifest.source")
    _require_exact_keys(value, _SOURCE_KEYS, "run manifest.source")
    _require_git_oid(value["revision"], "run manifest.source.revision")
    _require_git_oid(value["tree"], "run manifest.source.tree")
    if value["clean"] is not True:
        raise HarnessError("run manifest.source must record a clean worktree")
    _require_sha256(
        value["status_sha256"],
        "run manifest.source.status_sha256",
    )
    _require_sha256(
        value["worktree_content_sha256"],
        "run manifest.source.worktree_content_sha256",
    )
    if value["status_sha256"] != hashlib.sha256(b"").hexdigest():
        raise HarnessError("clean source state must have an empty status hash")
    if not verify_current:
        return
    current = _capture_source_state(repository_root, baseline_relative)
    if not current["clean"]:
        raise HarnessError("current source worktree is dirty outside the baseline")
    if current["status_sha256"] != value["status_sha256"]:
        raise HarnessError("source status hash does not match the baseline")
    if (
        current["worktree_content_sha256"]
        != value["worktree_content_sha256"]
    ):
        raise HarnessError("source worktree content hash does not match the baseline")


def _validate_run_database(
    value: Any,
    *,
    fixture: dict[str, Any],
    expected_copy_path: str,
    label: str,
) -> None:
    _require_type(value, dict, label)
    _require_exact_keys(value, _DATABASE_RUN_KEYS, label)
    if value != {
        "fixture": fixture["id"],
        "source_path": fixture["path"],
        "copy_path": expected_copy_path,
        "before_sha256": fixture["sha256"],
        "after_sha256": fixture["sha256"],
        "sidecars_remaining": [],
        "cleanup": "removed",
    }:
        raise HarnessError(f"{label} database ownership record is invalid")


def _validate_process(value: Any, label: str) -> None:
    _require_type(value, dict, label)
    _require_exact_keys(value, _PROCESS_KEYS, label)
    _require_integer(value["returncode"], f"{label}.returncode")
    _require_type(value["timed_out"], bool, f"{label}.timed_out")
    if value["returncode"] != 0 or value["timed_out"] is not False:
        raise HarnessError(f"{label} does not describe a successful child")
    _require_integer(
        value["runner_elapsed_ns"],
        f"{label}.runner_elapsed_ns",
        minimum=1,
    )


def _expected_timing_schedule(
    workload_manifest: dict[str, Any],
) -> list[tuple[int, str, str]]:
    schedule = []
    for round_definition in workload_manifest["rounds"]:
        case_ids = list(EXPECTED_CASE_IDS)
        if round_definition["case_order"] == "reverse":
            case_ids.reverse()
        for case_id in case_ids:
            for engine in round_definition["engine_order"]:
                schedule.append((round_definition["index"], case_id, engine))
    return schedule


def _validate_run_manifest(
    value: Any,
    *,
    baseline_path: pathlib.Path,
    repository_root: pathlib.Path,
    workload_path: pathlib.Path,
    workload_manifest: dict[str, Any],
    verify_current_source: bool,
) -> dict[tuple[int, str, str], dict[str, Any]]:
    _require_type(value, dict, "run manifest")
    _require_exact_keys(value, _RUN_MANIFEST_KEYS, "run manifest")
    if (
        value["schema_version"] != SCHEMA_VERSION
        or value["workload_semantics_version"] != WORKLOAD_SEMANTICS_VERSION
        or value["completion_schema_version"] != COMPLETION_SCHEMA_VERSION
        or value["diagnostic_schema_version"] != DIAGNOSTIC_SCHEMA_VERSION
        or value["status"] != "complete"
    ):
        raise HarnessError("run manifest versions or status are invalid")
    expected_workload_ref = {
        "path": _repository_relative_path(
            workload_path,
            repository_root,
            "workload manifest",
        ),
        "sha256": _sha256(workload_path),
        "size_bytes": workload_path.stat().st_size,
    }
    if value["workload_manifest"] != expected_workload_ref:
        raise HarnessError("run manifest workload reference is invalid")
    baseline_relative = _repository_relative_path(
        baseline_path,
        repository_root,
        "baseline directory",
    )
    _validate_source_state(
        value["source"],
        repository_root=repository_root,
        baseline_relative=baseline_relative,
        verify_current=verify_current_source,
    )

    profile_path = _resolve_repository_file(
        repository_root,
        workload_manifest["sqlite_profile"],
        "workload manifest.sqlite_profile",
    )
    profile = _validate_profile(
        load_json_strict(profile_path),
        label="SQLite profile",
    )
    expected_sqlite = {
        "version": SQLITE_VERSION,
        "source_id": SQLITE_SOURCE_ID,
        "compile_options": profile["build"]["semantic_compile_options"],
        "sqlite3_c_sha256": profile["sqlite"]["sqlite3_c_sha256"],
        "sqlite3_h_sha256": profile["sqlite"]["sqlite3_h_sha256"],
        "library_role": "statically-linked-benchmark-oracle",
    }
    if value["sqlite"] != expected_sqlite:
        raise HarnessError("run manifest SQLite provenance is invalid")

    build = value["build"]
    _require_type(build, dict, "run manifest.build")
    _require_exact_keys(build, _BUILD_PROVENANCE_KEYS, "run manifest.build")
    for key in _BUILD_PROVENANCE_KEYS:
        if type(build[key]) is not str:
            raise HarnessError(f"run manifest.build.{key} must be a string")
    if build["build_type"] != "Release":
        raise HarnessError("run manifest.build must be Release")
    for prefix, require_optimization in (
        ("c", True),
        ("cxx", True),
        ("linker", False),
    ):
        expected_effective = _effective_flags(
            build[f"{prefix}_base_flags"],
            build[f"{prefix}_release_flags"],
        )
        if build[f"{prefix}_effective_flags"] != expected_effective:
            raise HarnessError(
                f"run manifest.build.{prefix}_effective_flags is invalid"
            )
        _validate_benchmark_flags(
            expected_effective,
            require_optimization=require_optimization,
            label=f"run manifest {prefix} benchmark flags",
        )
    for key, require_optimization in (
        ("actual_sqlite_c_compile_flags", True),
        ("actual_modern_cxx_compile_flags", True),
        ("actual_harness_cxx_compile_flags", True),
        ("actual_timing_link_flags", False),
    ):
        commands = build[key].splitlines()
        if not commands or any(not command for command in commands):
            raise HarnessError(f"run manifest.build.{key} must be nonempty")
        for command in commands:
            _validate_benchmark_flags(
                command,
                require_optimization=require_optimization,
                label=f"run manifest.build.{key}",
            )

    host = value["host"]
    _require_type(host, dict, "run manifest.host")
    _require_exact_keys(host, _HOST_KEYS, "run manifest.host")
    for key in _HOST_KEYS - {"logical_cpu_count"}:
        _require_string(host[key], f"run manifest.host.{key}")
    _require_integer(
        host["logical_cpu_count"],
        "run manifest.host.logical_cpu_count",
        minimum=1,
    )
    if host["wall_timer"] != "steady_clock" or host["cpu_timer"] != (
        "CLOCK_PROCESS_CPUTIME_ID"
    ):
        raise HarnessError("run manifest host timer identities are invalid")

    expected_inputs = _collect_input_references(
        repository_root=repository_root,
        workload_path=workload_path,
        workload_manifest=workload_manifest,
    )
    if value["inputs"] != expected_inputs:
        raise HarnessError("run manifest input hashes are invalid")

    binaries = value["binaries"]
    _require_type(binaries, dict, "run manifest.binaries")
    _require_exact_keys(
        binaries,
        _BINARY_GROUP_KEYS,
        "run manifest.binaries",
    )
    for name, instrumentation in (("timing", False), ("diagnostic", True)):
        binary = binaries[name]
        _require_type(binary, dict, f"run manifest.binaries.{name}")
        _require_exact_keys(
            binary,
            _BINARY_KEYS,
            f"run manifest.binaries.{name}",
        )
        _require_string(
            binary["logical_name"],
            f"run manifest.binaries.{name}.logical_name",
        )
        _require_sha256(
            binary["sha256"],
            f"run manifest.binaries.{name}.sha256",
        )
        _require_integer(
            binary["size_bytes"],
            f"run manifest.binaries.{name}.size_bytes",
            minimum=1,
        )
        _validate_identity_report(
            binary["identity"],
            expected_source=value["source"],
            expected_semantic_options=workload_manifest[
                "sqlite_semantic_compile_options"
            ],
            instrumentation=instrumentation,
            label=f"run manifest.binaries.{name}.identity",
        )
    timing_build = dict(binaries["timing"]["identity"]["build"])
    diagnostic_build = dict(binaries["diagnostic"]["identity"]["build"])
    timing_build.pop("instrumentation")
    diagnostic_build.pop("instrumentation")
    if (
        timing_build != diagnostic_build
        or binaries["timing"]["identity"]["sqlite"]
        != binaries["diagnostic"]["identity"]["sqlite"]
    ):
        raise HarnessError("benchmark binary identities are not matched")
    if (
        binaries["timing"]["logical_name"]
        == binaries["diagnostic"]["logical_name"]
        or binaries["timing"]["sha256"] == binaries["diagnostic"]["sha256"]
    ):
        raise HarnessError("benchmark binary artifacts must be distinct")
    if value["schedule"] != workload_manifest["rounds"]:
        raise HarnessError("run manifest schedule is invalid")

    fixture_by_id = {
        fixture["id"]: fixture for fixture in workload_manifest["fixtures"]
    }
    case_by_id = {case["id"]: case for case in workload_manifest["cases"]}
    timing_schedule = _expected_timing_schedule(workload_manifest)
    timing_runs = value["timing_runs"]
    _require_type(timing_runs, list, "run manifest.timing_runs")
    if len(timing_runs) != len(timing_schedule):
        raise HarnessError("run manifest timing run count is invalid")
    timing_reports = {}
    for ordinal, (record, expected) in enumerate(
        zip(timing_runs, timing_schedule, strict=True)
    ):
        round_index, case_id, engine = expected
        label = f"run manifest.timing_runs[{ordinal}]"
        _require_type(record, dict, label)
        _require_exact_keys(record, _TIMING_RUN_KEYS, label)
        if (
            record["ordinal"] != ordinal
            or record["round"] != round_index
            or record["case"] != case_id
            or record["engine"] != engine
        ):
            raise HarnessError(f"{label} identity or order is invalid")
        expected_copy = (
            f"scratch/{ordinal:03d}-round-{round_index}-{case_id}-{engine}.db"
        )
        expected_argv = [
            binaries["timing"]["logical_name"],
            "run",
            engine,
            case_id,
            expected_copy,
            "baseline",
        ]
        if record["logical_argv"] != expected_argv:
            raise HarnessError(f"{label}.logical_argv is invalid")
        case = case_by_id[case_id]
        _validate_run_database(
            record["database"],
            fixture=fixture_by_id[case["fixture"]],
            expected_copy_path=expected_copy,
            label=f"{label}.database",
        )
        _validate_process(record["process"], f"{label}.process")
        stdout_path = _validate_artifact_reference(
            record["stdout"],
            baseline_path=baseline_path,
            label=f"{label}.stdout",
            maximum_size=4 * 1024 * 1024,
        )
        stderr_path = _validate_artifact_reference(
            record["stderr"],
            baseline_path=baseline_path,
            label=f"{label}.stderr",
            maximum_size=1024 * 1024,
        )
        if stderr_path.stat().st_size != 0:
            raise HarnessError(f"{label}.stderr must be empty")
        report = validate_raw_timing_report(
            load_json_strict(stdout_path),
            workload_manifest=workload_manifest,
            expected_engine=engine,
            expected_case=case_id,
            expected_run_kind="baseline",
        )
        measured_wall_ns = sum(
            repetition["wall_ns"] for repetition in report["repetitions"]
        )
        if record["process"]["runner_elapsed_ns"] < measured_wall_ns:
            raise HarnessError(
                f"{label}.process is shorter than the measured repetitions"
            )
        timing_reports[(round_index, case_id, engine)] = report

    diagnostic_schedule = [
        (case_id, engine)
        for case_id in EXPECTED_CASE_IDS
        for engine in ("modern", "sqlite")
    ]
    diagnostic_runs = value["diagnostic_runs"]
    _require_type(diagnostic_runs, list, "run manifest.diagnostic_runs")
    if len(diagnostic_runs) != len(diagnostic_schedule):
        raise HarnessError("run manifest diagnostic run count is invalid")
    for ordinal, (record, expected) in enumerate(
        zip(diagnostic_runs, diagnostic_schedule, strict=True)
    ):
        case_id, engine = expected
        label = f"run manifest.diagnostic_runs[{ordinal}]"
        _require_type(record, dict, label)
        _require_exact_keys(record, _DIAGNOSTIC_RUN_KEYS, label)
        if (
            record["ordinal"] != ordinal
            or record["case"] != case_id
            or record["engine"] != engine
        ):
            raise HarnessError(f"{label} identity or order is invalid")
        expected_copy = f"scratch/{ordinal:03d}-{case_id}-{engine}.db"
        expected_argv = [
            binaries["diagnostic"]["logical_name"],
            "run",
            engine,
            case_id,
            expected_copy,
            "diagnostic",
        ]
        if record["logical_argv"] != expected_argv:
            raise HarnessError(f"{label}.logical_argv is invalid")
        case = case_by_id[case_id]
        _validate_run_database(
            record["database"],
            fixture=fixture_by_id[case["fixture"]],
            expected_copy_path=expected_copy,
            label=f"{label}.database",
        )
        _validate_process(record["process"], f"{label}.process")
        stdout_path = _validate_artifact_reference(
            record["stdout"],
            baseline_path=baseline_path,
            label=f"{label}.stdout",
            maximum_size=4 * 1024 * 1024,
        )
        stderr_path = _validate_artifact_reference(
            record["stderr"],
            baseline_path=baseline_path,
            label=f"{label}.stderr",
            maximum_size=1024 * 1024,
        )
        if stderr_path.stat().st_size != 0:
            raise HarnessError(f"{label}.stderr must be empty")
        validate_raw_diagnostic_report(
            load_json_strict(stdout_path),
            workload_manifest=workload_manifest,
            expected_engine=engine,
            expected_case=case_id,
        )
    return timing_reports


def validate_baseline_directory(
    *,
    baseline_path: pathlib.Path,
    repository_root: pathlib.Path,
    workload_path: pathlib.Path,
    verify_current_source: bool = False,
) -> dict[str, Any]:
    baseline_path = baseline_path.resolve()
    repository_root = repository_root.resolve()
    workload_path = workload_path.resolve()
    if not baseline_path.is_dir():
        raise HarnessError("baseline path is not a directory")
    _repository_relative_path(
        baseline_path,
        repository_root,
        "baseline directory",
    )
    workload_manifest = validate_workload_manifest(
        load_json_strict(workload_path),
        repository_root=repository_root,
        manifest_path=workload_path,
    )
    run_manifest_path = baseline_path / "run-manifest.json"
    aggregate_path = baseline_path / "aggregate.json"
    try:
        run_manifest_size = run_manifest_path.stat().st_size
        aggregate_size = aggregate_path.stat().st_size
    except OSError as error:
        raise HarnessError(f"cannot inspect baseline reports: {error}") from error
    if run_manifest_size > 4 * 1024 * 1024:
        raise HarnessError("run manifest exceeds its size limit")
    if aggregate_size > 4 * 1024 * 1024:
        raise HarnessError("aggregate report exceeds its size limit")
    run_manifest = load_json_strict(run_manifest_path)
    timing_reports = _validate_run_manifest(
        run_manifest,
        baseline_path=baseline_path,
        repository_root=repository_root,
        workload_path=workload_path,
        workload_manifest=workload_manifest,
        verify_current_source=verify_current_source,
    )
    aggregate = validate_aggregate_report(
        load_json_strict(aggregate_path),
        workload_manifest=workload_manifest,
        timing_reports=timing_reports,
        expected_run_manifest_path="run-manifest.json",
        expected_run_manifest_sha256=_sha256(run_manifest_path),
    )

    referenced = {
        "aggregate.json",
        "run-manifest.json",
    }
    for group in ("timing_runs", "diagnostic_runs"):
        for record in run_manifest[group]:
            referenced.add(record["stdout"]["path"])
            referenced.add(record["stderr"]["path"])
    actual = {
        path.relative_to(baseline_path).as_posix()
        for path in baseline_path.rglob("*")
        if path.is_file()
    }
    if actual != referenced:
        raise HarnessError("baseline directory contains missing or extra artifacts")
    if len(actual) > MAX_BASELINE_ARTIFACTS:
        raise HarnessError("baseline directory exceeds its artifact count limit")
    if ENFORCE_GUARD_ON_VALIDATION:
        enforce_performance_guard(aggregate)
    return aggregate


def _build_parser() -> argparse.ArgumentParser:
    parser = _ArgumentParser(
        description="Generate and validate the pinned read-performance baseline."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_workloads = subparsers.add_parser(
        "validate-workloads",
        help="validate the pinned workload manifest and fixture hashes",
    )
    validate_workloads.add_argument(
        "--repository-root",
        type=pathlib.Path,
        required=True,
    )
    validate_workloads.add_argument(
        "--workloads",
        type=pathlib.Path,
        required=True,
    )
    create_fixture_parser = subparsers.add_parser(
        "create-fixture",
        help="create and validate one canonical read-performance fixture",
    )
    create_fixture_parser.add_argument(
        "--profile",
        type=pathlib.Path,
        required=True,
    )
    create_fixture_parser.add_argument(
        "--sqlite-library",
        type=pathlib.Path,
        required=True,
    )
    create_fixture_parser.add_argument(
        "--sqlite-c",
        type=pathlib.Path,
        required=True,
    )
    create_fixture_parser.add_argument(
        "--sqlite-h",
        type=pathlib.Path,
        required=True,
    )
    create_fixture_parser.add_argument(
        "--sql",
        type=pathlib.Path,
        required=True,
    )
    create_fixture_parser.add_argument(
        "--fixture",
        choices=("fit", "pressure"),
        required=True,
    )
    create_fixture_parser.add_argument(
        "--output",
        type=pathlib.Path,
        required=True,
    )
    generate_parser = subparsers.add_parser(
        "generate-baseline",
        help="run and preserve the complete pinned read-performance baseline",
    )
    for name in (
        "repository-root",
        "workloads",
        "timing-binary",
        "diagnostic-binary",
        "sqlite-c",
        "sqlite-h",
        "output",
    ):
        generate_parser.add_argument(
            f"--{name}",
            type=pathlib.Path,
            required=True,
        )
    validate_parser = subparsers.add_parser(
        "validate-baseline",
        help="strictly validate a committed read-performance baseline",
    )
    validate_parser.add_argument(
        "--repository-root",
        type=pathlib.Path,
        required=True,
    )
    validate_parser.add_argument(
        "--workloads",
        type=pathlib.Path,
        required=True,
    )
    validate_parser.add_argument(
        "--baseline",
        type=pathlib.Path,
        required=True,
    )
    return parser


def _run_command(arguments: argparse.Namespace) -> None:
    if arguments.command == "validate-workloads":
        repository_root = arguments.repository_root.resolve()
        workload_path = arguments.workloads.resolve()
        validate_workload_manifest(
            load_json_strict(workload_path),
            repository_root=repository_root,
            manifest_path=workload_path,
        )
        return
    if arguments.command == "create-fixture":
        metadata = create_fixture(
            profile_path=arguments.profile.resolve(),
            sqlite_library_path=arguments.sqlite_library.resolve(),
            sqlite_c_path=arguments.sqlite_c.resolve(),
            sqlite_h_path=arguments.sqlite_h.resolve(),
            sql_path=arguments.sql.resolve(),
            fixture_id=arguments.fixture,
            output_path=arguments.output.resolve(),
        )
        print(json.dumps(metadata, sort_keys=True, separators=(",", ":")))
        return
    if arguments.command == "generate-baseline":
        aggregate = generate_baseline(
            repository_root=arguments.repository_root,
            workload_path=arguments.workloads,
            timing_binary_path=arguments.timing_binary,
            diagnostic_binary_path=arguments.diagnostic_binary,
            sqlite_c_path=arguments.sqlite_c,
            sqlite_h_path=arguments.sqlite_h,
            output_path=arguments.output,
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
        return
    if arguments.command == "validate-baseline":
        validate_baseline_directory(
            baseline_path=arguments.baseline,
            repository_root=arguments.repository_root,
            workload_path=arguments.workloads,
        )
        return
    raise HarnessError(f"unsupported command: {arguments.command}")


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    try:
        arguments = parser.parse_args(argv)
        _run_command(arguments)
    except _ParseFailure as error:
        print(error, file=sys.stderr)
        return 1
    except HarnessError as error:
        print(f"read performance harness error: {error}", file=sys.stderr)
        return 1
    except PerformanceMismatch as error:
        print(f"read performance mismatch: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
