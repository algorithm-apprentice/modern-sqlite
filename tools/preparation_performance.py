#!/usr/bin/env python3

from __future__ import annotations

import argparse
import functools
import hashlib
import pathlib
import sys
from typing import Any

if __package__:
    from tools import distinct_compound_performance as contracts
    from tools import prepare_native_parser as adapter
    from tools import read_performance as common
else:
    import distinct_compound_performance as contracts
    import prepare_native_parser as adapter
    import read_performance as common


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = "tests/fixtures/order_by_performance/ordered.db"
SHARED_WORKLOADS = "tests/performance/read-workloads-v1.json"
MINIMUM_WALL_NS = 5_000_000
WARMUP_ITERATIONS = 8
SMOKE_ITERATIONS = 64
DIAGNOSTIC_ITERATIONS = 8
PHASES = ("parse", "prepare")
ENGINES = ("modern", "sqlite")
HarnessError = common.HarnessError
require_exact = contracts._require_exact_typed
INPUT_PATHS = (
    "CMakeLists.txt", "project/manifest.json", "project/verification-policy.json",
    "benchmarks/read_performance.cpp", "benchmarks/read_performance_build_config.hpp.in",
    "benchmarks/preparation_performance.cpp", "benchmarks/native_parser_adapter.c",
    "benchmarks/native_parser_adapter.h", "benchmarks/preparation_performance_build_config.hpp.in",
    "tools/read_performance.py", "tools/preparation_performance.py",
    "tools/prepare_native_parser.py", "tools/distinct_compound_performance.py",
    "tools/order_by_performance.py", SHARED_WORKLOADS,
    "tests/compatibility/sqlite-oracle-profile-v1.json",
    "tests/fixtures/order_by_performance/ordered.sql", FIXTURE,
)
PREPARATION_TARGETS = (
    "modern_sqlite_preparation_benchmark", "modern_sqlite_preparation_diagnostics",
    "modern_sqlite_native_parser_benchmark", "modern_sqlite_native_parser_diagnostics",
)


def expected_cases() -> dict[str, Any]:
    def wide(columns: int, malformed: bool = False) -> str:
        return "SELECT " + ",".join(["id"] * columns) + (", FROM items" if malformed else " FROM items")

    def deep(depth: int, malformed: bool = False) -> str:
        return "SELECT " + "(" * depth + "id" + ("" if malformed else ")" * depth + " FROM items WHERE id=?1")

    definitions = (
        ("short", "SELECT id FROM items WHERE id=?1", True, 1, 1, 65_536),
        ("wide-128", wide(128), True, 128, 0, 2_048),
        ("wide-512", wide(512), True, 512, 0, 512),
        ("deep-64", deep(64), True, 1, 1, 16_384),
        ("deep-256", deep(256), True, 1, 1, 4_096),
        ("malformed-short", "SELECT (id+ FROM items", False, 0, 0, 131_072),
        ("malformed-wide", wide(128, True), False, 0, 0, 2_048),
        ("malformed-deep", deep(256, True), False, 0, 0, 16_384),
    )
    cases = []
    for case_id, sql, accepted, columns, parameters, iterations in definitions:
        error_tail = 0 if accepted else (sql.index("FROM") + 4 if "FROM" in sql else len(sql))
        cases.append({
            "id": case_id, "sql": sql, "accepted": accepted, "columns": columns,
            "parameters": parameters, "error_tail": error_tail,
            "measured_iterations": iterations,
        })
    return {
        "schema_version": 1, "minimum_wall_ns": MINIMUM_WALL_NS,
        "warmup_iterations": WARMUP_ITERATIONS,
        "smoke_iterations": SMOKE_ITERATIONS,
        "diagnostic_iterations": DIAGNOSTIC_ITERATIONS, "cases": cases,
    }


def case_by_id(case_id: str) -> dict[str, Any]:
    for case in expected_cases()["cases"]:
        if case["id"] == case_id:
            return case
    raise HarnessError(f"unknown preparation case: {case_id}")


def reference_role(phase: str) -> str:
    if phase not in PHASES:
        raise HarnessError("preparation phase must be parse or prepare")
    return "derived-select-syntax" if phase == "parse" else "unmodified-public-prepare"


def configuration(phase: str) -> dict[str, Any]:
    reference_role(phase)
    if phase == "parse":
        return {
            "catalog": False, "statement_scope": "SELECT", "modern_recursion_limit": 512,
            "native_grammar": "Lemon", "maximum_columns": 2000, "native_lookaside": "default",
        }
    return {
        "cache_pages": 512, "journal_mode": "delete", "mmap_bytes": 0, "page_size": 4096,
        "query_only": True, "synchronous": "full", "temp_store": "memory", "thread_mode": "single",
    }


@functools.lru_cache(maxsize=128)
def _work_digest(case_id: str, phase: str, iterations: int) -> str:
    case = case_by_id(case_id)
    consumed = len(case["sql"]) if case["accepted"] else (case["error_tail"] if phase == "parse" else 0)
    columns = case["columns"] if phase == "prepare" else 0
    parameters = case["parameters"] if phase == "prepare" else 0
    record = (b"A" if case["accepted"] else b"E") + b"".join(
        value.to_bytes(8, "little") for value in (consumed, columns, parameters)
    )
    return f"{common._fnv1a64(record * iterations):016x}"


def expected_work(case_id: str, phase: str, iterations: int) -> dict[str, Any]:
    reference_role(phase)
    case = case_by_id(case_id)
    return {
        "operations": iterations, "items": iterations, "rows": 0,
        "bytes": len(case["sql"].encode()) * iterations,
        "result_hits": iterations if case["accepted"] else 0,
        "result_misses": 0 if case["accepted"] else iterations,
        "digest": _work_digest(case_id, phase, iterations),
    }


def timing_schedule() -> list[tuple[int, str, str]]:
    shared = common.load_json_strict(REPOSITORY_ROOT / SHARED_WORKLOADS)
    case_ids = tuple(f"{phase}:{case['id']}" for phase in PHASES for case in expected_cases()["cases"])
    return common._expected_timing_schedule(shared, case_ids)


def validate_report(
    value: Any, *, engine: str, phase: str, case_id: str, diagnostic: bool,
) -> dict[str, Any]:
    if engine not in ENGINES:
        raise HarnessError("unknown preparation engine")
    case = case_by_id(case_id)
    keys = {
        "schema_version", "mode", "run_kind", "engine", "phase", "case", "reference_role",
        "build", "completion", "effective_configuration", "warmup", "repetitions",
    } | ({"work", "counters"} if diagnostic else set())
    common._require_type(value, dict, "preparation report")
    common._require_exact_keys(value, keys, "preparation report")
    for key, expected in {
        "schema_version": 1, "mode": "diagnostic" if diagnostic else "timing",
        "run_kind": "diagnostic" if diagnostic else "baseline", "engine": engine,
        "phase": phase, "case": case_id, "reference_role": reference_role(phase),
        "effective_configuration": configuration(phase),
        "warmup": expected_work(case_id, phase, WARMUP_ITERATIONS),
    }.items():
        require_exact(value[key], expected, f"preparation {key}")
    common._validate_build(value["build"], instrumentation=diagnostic, label="preparation build")
    iterations = DIAGNOSTIC_ITERATIONS if diagnostic else case["measured_iterations"]
    attempts = WARMUP_ITERATIONS + 2 + iterations * (1 if diagnostic else 3)
    require_exact(value["completion"], {
        "status": "complete", "parse_calls": attempts if phase == "parse" else 0,
        "prepare_calls": attempts if phase == "prepare" else 0,
        "statement_finalizes": attempts if phase == "prepare" and case["accepted"] else 0,
        "statement_steps": 0, "statement_resets": 0,
    }, "preparation completion")
    common._require_type(value["repetitions"], list, "preparation repetitions")
    if len(value["repetitions"]) != (0 if diagnostic else 3):
        raise HarnessError("preparation repetition count is not pinned")
    work = expected_work(case_id, phase, iterations)
    for index, repetition in enumerate(value["repetitions"]):
        common._require_type(repetition, dict, "preparation repetition")
        common._require_exact_keys(repetition, {"index", "wall_ns", "cpu_ns", "work"}, "preparation repetition")
        require_exact(repetition["index"], index, "preparation repetition index")
        require_exact(repetition["work"], work, "preparation repetition work")
        common._require_integer(repetition["wall_ns"], "preparation wall_ns", minimum=MINIMUM_WALL_NS)
        common._require_integer(repetition["cpu_ns"], "preparation cpu_ns", minimum=1)
    if diagnostic:
        require_exact(value["work"], work, "preparation diagnostic work")
        counters = value["counters"]
        common._require_type(counters, dict, "preparation counters")
        common._require_exact_keys(counters, {"modern", "sqlite"}, "preparation counters")
        for name in ENGINES:
            common._require_type(counters[name], dict, f"preparation {name} counters")
        require_exact(counters["sqlite" if engine == "modern" else "modern"], {}, "inactive counters")
        names = common.MODERN_COUNTER_NAMES if engine == "modern" else (
            "malloc_count_current", "malloc_count_highwater", "memory_bytes_current",
            "memory_bytes_highwater", "lookaside_used", "lookaside_highwater", "lookaside_hits",
        )
        common._require_exact_keys(counters[engine], set(names), "active preparation counters")
        for name, count in counters[engine].items():
            common._require_integer(count, f"preparation counter {name}", minimum=0)
        if engine == "modern":
            if counters[engine]["allocations"] == 0 or counters[engine]["pages_written"] != 0:
                raise HarnessError("Modern preparation allocation/write control failed")
        elif (
            counters[engine]["lookaside_used"] != 0
            or counters[engine]["malloc_count_highwater"] < counters[engine]["malloc_count_current"]
            or counters[engine]["memory_bytes_highwater"] < counters[engine]["memory_bytes_current"]
        ):
            raise HarnessError("native preparation cleanup/allocation control failed")
    return value


def _invoke(binary: pathlib.Path, arguments: list[str], root: pathlib.Path) -> dict[str, Any]:
    result = common.run_bounded(
        [str(binary), *arguments], cwd=root, timeout_seconds=30,
        stdout_limit=128 * 1024, stderr_limit=32 * 1024,
    )
    return {
        "stdout": result.stdout.decode("utf-8"), "stderr": result.stderr.decode("utf-8"),
        "stdout_sha256": hashlib.sha256(result.stdout).hexdigest(),
        "elapsed_ns": result.elapsed_ns, "returncode": result.returncode,
    }


def _report(record: dict[str, Any]) -> Any:
    common._require_type(record, dict, "raw preparation child")
    common._require_exact_keys(
        record, {"stdout", "stderr", "stdout_sha256", "elapsed_ns", "returncode"}, "raw preparation child"
    )
    require_exact(record["returncode"], 0, "preparation child exit")
    require_exact(record["stderr"], "", "preparation child stderr")
    common._require_integer(record["elapsed_ns"], "preparation child elapsed_ns", minimum=1)
    stdout = common._require_string(record["stdout"], "preparation child stdout")
    require_exact(
        record["stdout_sha256"], hashlib.sha256(stdout.encode()).hexdigest(), "preparation stdout SHA-256"
    )
    return common.load_json_bytes_strict(stdout.encode(), "raw preparation report")


def _build_provenance(root: pathlib.Path, timing: pathlib.Path, diagnostics: pathlib.Path) -> dict[str, Any]:
    build_directory = common._find_cmake_cache(timing).parent
    cache = common._parse_cmake_cache(build_directory / "CMakeCache.txt")
    pending = common._command_output(
        [cache["CMAKE_MAKE_PROGRAM"], "-C", str(build_directory), "-n",
         "modern_sqlite_preparation_benchmark", "modern_sqlite_preparation_diagnostics"],
        cwd=root, label="preparation target freshness",
    ).decode()
    if "ninja: no work to do." not in pending:
        raise HarnessError("preparation binaries have outstanding build work")
    reference_directory = pathlib.Path(cache["MODERN_SQLITE_PINNED_SQLITE_C"]).parent
    provenance = common._capture_build_provenance(
        repository_root=root, timing_binary_path=timing, diagnostic_binary_path=diagnostics,
        sqlite_source_directory=reference_directory,
    )
    provenance.pop("actual_harness_cxx_compile_flags")
    provenance.pop("actual_timing_link_flags")
    commands = common.load_json_strict(build_directory / "compile_commands.json")
    for target in PREPARATION_TARGETS + ("modern_sqlite_diagnostic_engine",):
        matches = [entry["command"] for entry in commands if f"CMakeFiles/{target}.dir/" in entry["command"]]
        if not matches or (target != "modern_sqlite_diagnostic_engine" and len(matches) != 1):
            raise HarnessError(f"expected one compile command for {target}")
        captured = set()
        for command in matches:
            flags = common._extract_compile_flags(
                command, repository_root=root, build_directory=build_directory,
                sqlite_source_directory=reference_directory,
            )
            common._validate_benchmark_flags(flags, require_optimization=True, label=target)
            diagnostic = target.endswith("diagnostics")
            if target == "modern_sqlite_diagnostic_engine":
                if "-DMODERN_SQLITE_ENABLE_INSTRUMENTATION=1" not in flags:
                    raise HarnessError("diagnostic engine lacks instrumentation")
            elif f"-DMODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS={int(diagnostic)}" not in flags:
                raise HarnessError(f"wrong diagnostic role in {target}")
            captured.add(flags)
        provenance[f"{target}_compile_flags"] = "\n".join(sorted(captured))
    for target in ("modern_sqlite_preparation_benchmark", "modern_sqlite_preparation_diagnostics"):
        graph = common._command_output(
            [cache["CMAKE_MAKE_PROGRAM"], "-C", str(build_directory), "-t", "commands", target],
            cwd=root, label=f"{target} command graph",
        ).decode()
        links = [line for line in graph.splitlines() if f" -o {target} " in line and " -c " not in line]
        if len(links) != 1:
            raise HarnessError(f"expected one link command for {target}")
        diagnostic = target.endswith("diagnostics")
        if ("modern_sqlite_diagnostic_engine" in links[0]) != diagnostic:
            raise HarnessError(f"{target} has the wrong ordinary/diagnostic engine")
        if f"modern_sqlite_native_parser_{'diagnostics' if diagnostic else 'benchmark'}" not in links[0]:
            raise HarnessError(f"{target} has the wrong private native adapter")
        flags = common._extract_link_flags(
            links[0], compiler_path=cache["CMAKE_CXX_COMPILER"], output_name=target,
            repository_root=root, build_directory=build_directory,
            sqlite_source_directory=reference_directory,
        )
        common._validate_benchmark_flags(flags, require_optimization=False, label=f"{target} link")
        provenance[f"{target}_link_command"] = common._normalize_actual_flag(
            links[0], repository_root=root, build_directory=build_directory,
            sqlite_source_directory=reference_directory,
        )
    return provenance


def _inputs(root: pathlib.Path) -> list[dict[str, Any]]:
    return [
        common._repository_input_reference(role=path, path=root / path, repository_root=root)
        for path in INPUT_PATHS
    ]


def _summary(runs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result = []
    for phase in PHASES:
        for case in expected_cases()["cases"]:
            engine_results = {}
            for engine in ENGINES:
                samples = [
                    (run["round"], repetition)
                    for run in runs if run["key"] == f"{phase}:{case['id']}" and run["engine"] == engine
                    for repetition in _report(run["raw"])["repetitions"]
                ]
                if len(samples) != 9:
                    raise HarnessError("preparation summary requires nine samples per engine")
                engine_results[engine] = {
                    metric: common.median_integer([sample[1][metric] for sample in samples])
                    for metric in ("wall_ns", "cpu_ns")
                }
                engine_results[engine]["rounds"] = [
                    {metric: common.median_integer([
                        sample[1][metric] for sample in samples if sample[0] == index
                    ]) for metric in ("wall_ns", "cpu_ns")}
                    for index in range(3)
                ]
            ratios = {
                metric: common.exact_ratio(engine_results["modern"][metric], engine_results["sqlite"][metric])
                for metric in ("wall_ns", "cpu_ns")
            }
            result.append({
                "phase": phase, "case": case["id"], "engines": engine_results, "ratios": ratios,
            })
    return result


def generate(root: pathlib.Path, timing: pathlib.Path, diagnostics: pathlib.Path, output: pathlib.Path) -> dict[str, Any]:
    root, timing, diagnostics, output = (path.resolve() for path in (root, timing, diagnostics, output))
    source = common._capture_source_state(root, "")
    if not source["clean"]:
        raise HarnessError("fresh preparation evidence requires a clean source worktree")
    if output.exists():
        raise HarnessError("preparation evidence output already exists")
    if timing == diagnostics:
        raise HarnessError("timing and diagnostic binaries must be distinct")
    cache = common._parse_cmake_cache(common._find_cmake_cache(timing))
    reference = pathlib.Path(cache["MODERN_SQLITE_PINNED_SQLITE_C"])
    header = pathlib.Path(cache["MODERN_SQLITE_PINNED_SQLITE_H"])
    generated = timing.parent / "generated/native_parser_sqlite.c"
    if common._sha256(reference) != adapter.SQLITE_SOURCE_SHA256:
        raise HarnessError("original native source is not pinned")
    expected_generated = adapter.transform_source(reference.read_text()).encode()
    if generated.read_bytes() != expected_generated:
        raise HarnessError("native syntax copy is not the exact two-callback transformation")
    shared = common.load_json_strict(root / SHARED_WORKLOADS)
    common.validate_workload_manifest(shared, repository_root=root, manifest_path=root / SHARED_WORKLOADS)
    profile = common.load_json_strict(root / shared["sqlite_profile"])
    require_exact(common._sha256(header), profile["sqlite"]["sqlite3_h_sha256"], "native header SHA-256")
    build = _build_provenance(root, timing, diagnostics)
    archive: dict[str, Any] = {
        "schema_version": 1, "status": "in_progress", "purpose": "baseline-not-a-disposition",
        "source": source, "build": build, "host": common._capture_host_provenance(root),
        "inputs": _inputs(root), "workloads": expected_cases(), "binaries": {},
        "controls": {}, "timing_runs": [], "diagnostic_runs": [],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        for role, binary in (("timing", timing), ("diagnostics", diagnostics)):
            identity = _report(_invoke(binary, ["identity"], root))
            common._validate_identity_report(
                identity["measurement"], expected_source=source,
                expected_semantic_options=shared["sqlite_semantic_compile_options"],
                instrumentation=role == "diagnostics", label=f"preparation {role} identity",
            )
            require_exact(identity["native_syntax_adapter"], {
                "role": "derived-select-syntax", "semantic_callback_guards": 2,
                "symbol_linkage": "translation-unit-local", "source_sha256": common._sha256(reference),
                "generated_sha256": hashlib.sha256(expected_generated).hexdigest(),
                "source_id": common.SQLITE_SOURCE_ID,
                "diagnostic_allocator_wrappers": role == "diagnostics",
            }, "native syntax identity")
            archive["binaries"][role] = common._binary_reference(path=binary, identity=identity)
            archive["controls"][role] = _invoke(binary, ["self-test"], root)
            _report(archive["controls"][role])
        require_exact(_report(_invoke(timing, ["cases"], root)), expected_cases(), "compiled preparation cases")
        for index, key, engine in timing_schedule():
            phase, case_id = key.split(":")
            run = {"round": index, "key": key, "engine": engine, "raw": _invoke(
                timing, ["run", engine, phase, case_id, str(root / FIXTURE), "baseline"], root
            )}
            archive["timing_runs"].append(run)
            validate_report(_report(run["raw"]), engine=engine, phase=phase, case_id=case_id, diagnostic=False)
        for phase in PHASES:
            for case in expected_cases()["cases"]:
                for engine in ENGINES:
                    run = {"key": f"{phase}:{case['id']}", "engine": engine, "raw": _invoke(
                        diagnostics, ["run", engine, phase, case["id"], str(root / FIXTURE), "diagnostic"], root
                    )}
                    archive["diagnostic_runs"].append(run)
                    validate_report(_report(run["raw"]), engine=engine, phase=phase, case_id=case["id"], diagnostic=True)
        require_exact(common._capture_source_state(root, ""), source, "source after preparation measurement")
        require_exact(_inputs(root), archive["inputs"], "preparation inputs after measurement")
        for role, binary in (("timing", timing), ("diagnostics", diagnostics)):
            require_exact(common._sha256(binary), archive["binaries"][role]["sha256"], f"{role} binary after measurement")
        archive["summary"] = _summary(archive["timing_runs"])
        archive["status"] = "complete"
        validate_archive(archive, root, verify_current_source=True)
    except (HarnessError, OSError, UnicodeError) as error:
        archive["status"] = "failed"
        archive["error"] = str(error)
        if isinstance(error, common.ChildExecutionError):
            archive["failed_child"] = {
                "stdout": error.stdout.decode("utf-8", errors="replace"),
                "stderr": error.stderr.decode("utf-8", errors="replace"),
                "returncode": error.returncode, "elapsed_ns": error.elapsed_ns,
                "timed_out": error.timed_out,
            }
        common._write_new_json(output, archive)
        raise
    common._write_new_json(output, archive)
    return archive


def validate_archive(value: Any, root: pathlib.Path, *, verify_current_source: bool) -> dict[str, Any]:
    common._require_type(value, dict, "preparation archive")
    common._require_exact_keys(value, {
        "schema_version", "status", "purpose", "source", "build", "host", "inputs",
        "workloads", "binaries", "controls", "timing_runs", "diagnostic_runs", "summary",
    }, "preparation archive")
    for key, expected in {
        "schema_version": 1, "status": "complete", "purpose": "baseline-not-a-disposition",
        "workloads": expected_cases(),
    }.items():
        require_exact(value[key], expected, f"preparation archive {key}")
    source = value["source"]
    common._require_type(source, dict, "recorded preparation source")
    common._require_exact_keys(source, common._SOURCE_KEYS, "recorded preparation source")
    for name in ("revision", "tree"):
        common._require_git_oid(source[name], f"recorded preparation {name}")
    require_exact(source["clean"], True, "recorded preparation clean source")
    require_exact(source["status_sha256"], hashlib.sha256(b"").hexdigest(), "clean source status SHA-256")
    common._require_sha256(source["worktree_content_sha256"], "recorded worktree SHA-256")
    inputs = value["inputs"]
    common._require_type(inputs, list, "recorded preparation inputs")
    if len(inputs) != len(INPUT_PATHS):
        raise HarnessError("recorded preparation input population is not pinned")
    for reference, path in zip(inputs, INPUT_PATHS, strict=True):
        common._require_type(reference, dict, "recorded preparation input")
        common._require_exact_keys(reference, common._INPUT_KEYS, "recorded preparation input")
        require_exact(reference["role"], path, "preparation input role")
        require_exact(reference["path"], path, "preparation input path")
        common._require_sha256(reference["sha256"], "preparation input SHA-256")
        common._require_integer(reference["size_bytes"], "preparation input size", minimum=1)
    build = value["build"]
    common._require_type(build, dict, "recorded preparation build")
    build_keys = (
        common._BUILD_PROVENANCE_KEYS - {"actual_harness_cxx_compile_flags", "actual_timing_link_flags"}
    ) | {f"{target}_compile_flags" for target in PREPARATION_TARGETS + ("modern_sqlite_diagnostic_engine",)} | {
        "modern_sqlite_preparation_benchmark_link_command", "modern_sqlite_preparation_diagnostics_link_command",
    }
    common._require_exact_keys(build, build_keys, "recorded preparation build")
    for name, text in build.items():
        common._require_type(text, str, f"recorded preparation build {name}")
        if name in {"c_effective_flags", "cxx_effective_flags"} or name.endswith("_compile_flags"):
            common._validate_benchmark_flags(text, require_optimization=True, label=name)
    for name in ("cmake_version", "c_compiler", "c_compiler_version", "cxx_compiler", "cxx_compiler_version"):
        common._require_string(build[name], f"recorded preparation build {name}")
    require_exact(build["build_type"], "Release", "recorded preparation build type")
    require_exact(build["generator"], "Ninja", "recorded preparation generator")
    for target in PREPARATION_TARGETS:
        diagnostic = target.endswith("diagnostics")
        if f"-DMODERN_SQLITE_READ_PERFORMANCE_DIAGNOSTICS={int(diagnostic)}" not in build[f"{target}_compile_flags"]:
            raise HarnessError("recorded preparation target diagnostic role is invalid")
        if "native_parser" in target:
            for definition in ("-DSQLITE_DQS=3", "-DSQLITE_THREADSAFE=0", "-DNDEBUG"):
                if definition not in build[f"{target}_compile_flags"]:
                    raise HarnessError("recorded native syntax compiler profile is invalid")
    for suffix in ("benchmark", "diagnostics"):
        command = build[f"modern_sqlite_preparation_{suffix}_link_command"]
        common._validate_benchmark_flags(command, require_optimization=False, label=f"{suffix} link")
        if ("modern_sqlite_diagnostic_engine" in command) != (suffix == "diagnostics"):
            raise HarnessError("recorded preparation engine linkage is invalid")
        if f"modern_sqlite_native_parser_{suffix}" not in command:
            raise HarnessError("recorded preparation adapter linkage is invalid")
    host = value["host"]
    common._require_type(host, dict, "recorded preparation host")
    common._require_exact_keys(host, common._HOST_KEYS, "recorded preparation host")
    for name in common._HOST_KEYS - {"logical_cpu_count"}:
        common._require_string(host[name], f"recorded preparation host {name}")
    common._require_integer(host["logical_cpu_count"], "recorded preparation CPU count", minimum=1)
    require_exact(host["wall_timer"], "steady_clock", "preparation wall timer")
    require_exact(host["cpu_timer"], "CLOCK_PROCESS_CPUTIME_ID", "preparation CPU timer")
    if verify_current_source:
        require_exact(common._capture_source_state(root, ""), value["source"], "current preparation source")
        require_exact(_inputs(root), value["inputs"], "current preparation inputs")
    for key in ("binaries", "controls"):
        common._require_type(value[key], dict, f"preparation {key}")
        common._require_exact_keys(value[key], {"timing", "diagnostics"}, f"preparation {key}")
    shared = common.load_json_strict(root / SHARED_WORKLOADS)
    for role in ("timing", "diagnostics"):
        binary = value["binaries"][role]
        common._require_type(binary, dict, f"{role} binary")
        common._require_exact_keys(binary, {"logical_name", "sha256", "size_bytes", "identity"}, f"{role} binary")
        require_exact(binary["logical_name"], f"modern_sqlite_preparation_{'benchmark' if role == 'timing' else 'diagnostics'}", "preparation binary name")
        common._require_sha256(binary["sha256"], f"{role} binary SHA-256")
        common._require_integer(binary["size_bytes"], f"{role} binary size", minimum=1)
        identity = binary["identity"]
        common._require_type(identity, dict, "preparation identity")
        common._require_exact_keys(identity, {"measurement", "native_syntax_adapter"}, "preparation identity")
        common._validate_identity_report(
            identity["measurement"], expected_source=value["source"],
            expected_semantic_options=shared["sqlite_semantic_compile_options"],
            instrumentation=role == "diagnostics", label=f"recorded preparation {role} identity",
        )
        require_exact(identity["native_syntax_adapter"], {
            "role": "derived-select-syntax", "semantic_callback_guards": 2,
            "symbol_linkage": "translation-unit-local",
            "source_sha256": adapter.SQLITE_SOURCE_SHA256,
            "generated_sha256": adapter.SYNTAX_SOURCE_SHA256,
            "source_id": common.SQLITE_SOURCE_ID, "diagnostic_allocator_wrappers": role == "diagnostics",
        }, "recorded native syntax identity")
        control = _report(value["controls"][role])
        common._require_type(control, dict, "recorded preparation controls")
        common._require_exact_keys(control, {
            "status", "no_vdbe_or_schema_mutation", "same_context_retry",
            "ordinary_prepare_still_rejects_unknown_table", "unsupported_statement_controls",
            "balanced_cleanup_iterations", "oom_failures",
        }, "recorded preparation controls")
        for name in ("no_vdbe_or_schema_mutation", "same_context_retry", "ordinary_prepare_still_rejects_unknown_table"):
            require_exact(control[name], True, f"native control {name}")
        require_exact(control["status"], "complete", "native control status")
        common._require_integer(control["unsupported_statement_controls"], "unsupported controls", minimum=5)
        common._require_integer(control["balanced_cleanup_iterations"], "cleanup iterations", minimum=1000)
        if role == "diagnostics":
            common._require_integer(control["oom_failures"], "OOM controls", minimum=1)
        else:
            require_exact(control["oom_failures"], 0, "ordinary native allocator control")
    for key in ("timing_runs", "diagnostic_runs"):
        common._require_type(value[key], list, f"preparation {key}")
        for run in value[key]:
            common._require_type(run, dict, "preparation run")
            common._require_exact_keys(
                run, {"key", "engine", "raw"} | ({"round"} if key == "timing_runs" else set()), "preparation run"
            )
    expected_schedule = timing_schedule()
    require_exact(
        [(run["round"], run["key"], run["engine"]) for run in value["timing_runs"]],
        expected_schedule, "preparation timing schedule",
    )
    for run in value["timing_runs"]:
        phase, case_id = run["key"].split(":")
        report = validate_report(_report(run["raw"]), engine=run["engine"], phase=phase, case_id=case_id, diagnostic=False)
        if run["raw"]["elapsed_ns"] < sum(repetition["wall_ns"] for repetition in report["repetitions"]):
            raise HarnessError("preparation child elapsed time is shorter than its measured work")
    expected_diagnostics = [(f"{phase}:{case['id']}", engine)
                            for phase in PHASES for case in expected_cases()["cases"] for engine in ENGINES]
    require_exact([(run["key"], run["engine"]) for run in value["diagnostic_runs"]],
                  expected_diagnostics, "preparation diagnostic schedule")
    for run in value["diagnostic_runs"]:
        phase, case_id = run["key"].split(":")
        validate_report(_report(run["raw"]), engine=run["engine"], phase=phase, case_id=case_id, diagnostic=True)
    require_exact(value["summary"], _summary(value["timing_runs"]), "preparation summary")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="Matched SELECT parse and prepare evidence")
    parser.add_argument("operation", choices=("generate", "validate-baseline", "validate-historical-baseline"))
    parser.add_argument("--repository-root", type=pathlib.Path, default=REPOSITORY_ROOT)
    parser.add_argument("--timing-binary", type=pathlib.Path, default=REPOSITORY_ROOT / "build/benchmark/modern_sqlite_preparation_benchmark")
    parser.add_argument("--diagnostic-binary", type=pathlib.Path, default=REPOSITORY_ROOT / "build/benchmark/modern_sqlite_preparation_diagnostics")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--baseline", type=pathlib.Path)
    arguments = parser.parse_args()
    try:
        if arguments.operation == "generate":
            if arguments.output is None:
                raise HarnessError("generate requires --output")
            value = generate(arguments.repository_root, arguments.timing_binary, arguments.diagnostic_binary, arguments.output)
        else:
            if arguments.baseline is None:
                raise HarnessError("validation requires --baseline")
            value = validate_archive(
                common.load_json_strict(arguments.baseline), arguments.repository_root,
                verify_current_source=arguments.operation == "validate-baseline",
            )
        print(f"Validated {len(value['summary'])} matched preparation cases.")
    except (HarnessError, OSError, UnicodeError) as error:
        print(f"preparation performance: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
