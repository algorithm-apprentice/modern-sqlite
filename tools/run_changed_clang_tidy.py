#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools import verify

PRODUCTION_SOURCE_ROOT = (ROOT / "src").resolve()
SOURCE_SUFFIXES = {".cc", ".cpp", ".cxx"}
SHARD_COUNT = 3


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run clang-tidy only for changed translation units."
    )
    parser.add_argument("--base-ref", required=True)
    parser.add_argument("--build-directory", required=True, type=Path)
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--smoke-source", type=Path, action="append")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--reuse-report", type=Path)
    parser.add_argument(
        "--shard-index", type=int, choices=range(SHARD_COUNT),
        help="Select one of three single-process whole-translation-unit shards.",
    )
    args = parser.parse_args()
    if args.jobs is None:
        args.jobs = 1 if args.shard_index is not None else max(1, min(4, os.cpu_count() or 1))
    elif args.shard_index is not None and args.jobs != 1:
        parser.error("sharded analysis requires --jobs 1")
    if args.reuse_report is not None and args.report is None:
        parser.error("--reuse-report requires --report")
    return args


def changed_files(base_ref: str) -> list[Path]:
    result = subprocess.run(
        [
            "git",
            "diff",
            "--name-only",
            "--diff-filter=ACMR",
            f"{base_ref}...HEAD",
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return [Path(line) for line in result.stdout.splitlines() if line]


def compile_commands(build_directory: Path) -> dict[Path, str]:
    database_path = ROOT / build_directory / "compile_commands.json"
    entries = json.loads(database_path.read_text(encoding="utf-8"))
    build_root = (ROOT / build_directory).resolve()
    commands = {}
    for entry in entries:
        if "output" not in entry:
            continue
        directory = Path(entry["directory"])
        if not directory.is_absolute():
            raise ValueError(f"Compilation directory is not absolute: {directory}")
        source = Path(entry["file"])
        output = Path(entry["output"])
        source = (source if source.is_absolute() else directory / source).resolve()
        output = (output if output.is_absolute() else directory / output).resolve()
        commands[source] = str(output.relative_to(build_root))
    return commands


def is_production_source(path: Path) -> bool:
    try:
        path.resolve().relative_to(PRODUCTION_SOURCE_ROOT)
    except ValueError:
        return False
    return True


def partition_sources(sources: set[Path]) -> list[list[Path]]:
    partitions: list[list[Path]] = [[] for _ in range(SHARD_COUNT)]
    weights = [0] * SHARD_COUNT
    weighted = [(source, max(1, source.stat().st_size)) for source in sources]
    for source, weight in sorted(weighted, key=lambda item: (-item[1], item[0].as_posix())):
        owner = min(range(SHARD_COUNT), key=lambda index: (weights[index], index))
        partitions[owner].append(source)
        weights[owner] += weight
    return [sorted(partition) for partition in partitions]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def analysis_context(build: Path, assigned: list[Path], database: dict[Path, str]) -> dict:
    cache = verify.read_cache(build)
    if (
        Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != ROOT.resolve()
        or cache.get("CMAKE_GENERATOR") != "Ninja"
        or cache.get("MODERN_SQLITE_ENABLE_CLANG_TIDY", "").upper() not in {"ON", "TRUE", "1"}
    ):
        raise verify.VerificationError("analysis reports require this checkout's configured Ninja quality build")
    tools = {}
    for name in ("CMAKE_MAKE_PROGRAM", "CMAKE_CXX_COMPILER", "MODERN_SQLITE_CLANG_TIDY_EXECUTABLE"):
        requested = cache.get(name, "")
        found = shutil.which(requested) if requested else None
        if found is None:
            raise verify.VerificationError(f"analysis tool is unavailable: {name}")
        tools[name] = {"path": str(Path(found).resolve()), "sha256": digest(Path(found))}
    cmake = shutil.which("cmake")
    if cmake is None:
        raise verify.VerificationError("analysis requires CMake")
    tools["cmake"] = {"path": str(Path(cmake).resolve()), "sha256": digest(Path(cmake))}
    commands, objects, source_paths = {}, {}, {}
    for source in assigned:
        target = database[source].replace("\\", "/")
        relative = Path(target)
        if relative.is_absolute() or ".." in relative.parts:
            raise verify.VerificationError("analysis object target escapes its build owner")
        output = build / relative
        if (
            output.resolve() != output or not output.is_relative_to(build)
            or output.resolve() in objects.values()
        ):
            raise verify.VerificationError("analysis object is aliased or outside its build owner")
        objects[target] = output.resolve()
        text = subprocess.check_output(
            [tools["CMAKE_MAKE_PROGRAM"]["path"], "-C", str(build), "-t", "commands", target],
            text=True, timeout=10,
        )
        matches = []
        for line in text.splitlines():
            arguments = shlex.split(line)
            if "__run_co_compile" not in arguments or "--" not in arguments:
                continue
            if f"--source={source}" not in arguments:
                continue
            if (
                len(arguments) < 4 or arguments[1:3] != ["-E", "__run_co_compile"]
                or Path(arguments[0]).resolve() != Path(tools["cmake"]["path"])
            ):
                raise verify.VerificationError("selected target has a noncanonical analysis launcher")
            tidy = [value.removeprefix("--tidy=") for value in arguments if value.startswith("--tidy=")]
            if len(tidy) != 1 or Path(tidy[0].split(";")[0]).resolve() != Path(
                tools["MODERN_SQLITE_CLANG_TIDY_EXECUTABLE"]["path"]
            ):
                raise verify.VerificationError("selected target does not use the configured analyzer")
            matches.append(line)
        if len(matches) != 1:
            raise verify.VerificationError("selected target lacks one canonical compiler-plus-analyzer command")
        commands[target] = matches[0]
        source_paths[target] = str(source.resolve())
    configs = {str(path): digest(path) for path in (
        build / "CMakeCache.txt", build / "compile_commands.json", build / "build.ninja", ROOT / ".clang-tidy",
    )}
    rules = build / "CMakeFiles" / "rules.ninja"
    if rules.is_file():
        configs[str(rules)] = digest(rules)
    identities = sorted((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    configs.update({str(path): digest(path) for path in identities})
    generated_arguments = [shlex.split(command) for command in commands.values()]
    compiler_arguments = [
        arguments[arguments.index("--") + 1:] for arguments in generated_arguments
    ]
    full_mode = (
        len(identities) == 1
        and re.search(r'set\(CMAKE_CXX_COMPILER_ID "(GNU|Clang|AppleClang)"\)',
                      identities[0].read_text(encoding="utf-8")) is not None
        and not any(
            value.startswith("--launcher=")
            for arguments in generated_arguments for value in arguments[:arguments.index("--")]
        )
        and all(
            arguments and Path(arguments[0]).resolve() == Path(tools["CMAKE_CXX_COMPILER"]["path"])
            and "-MD" in arguments
            and not any(
                value.startswith(("@", "-Wp,", "-Xpreprocessor", "-Xclang", "-fmodule",
                                  "-ivfsoverlay", "--vfsoverlay", "-include-pch"))
                or value.startswith("-M") and value not in {"-MD", "-MF", "-MT", "-MQ", "-MP"}
                or value in {"--user-dependencies", "--write-user-dependencies"}
                for value in arguments
            )
            for arguments in compiler_arguments
        )
        and not cache.get("CMAKE_CXX_COMPILER_LAUNCHER")
    )
    return {
        "repository": str(ROOT.resolve()), "build_directory": str(build),
        "source_sha256": verify.repository_identity(ROOT, ignore_documentation=True),
        "environment_sha256": verify.environment_identity(),
        "tools": tools, "configuration_files": configs, "commands": commands, "sources": source_paths,
        "full_dependency_mode": bool(full_mode),
        "driver_sha256": digest(Path(__file__)), "verifier_sha256": digest(Path(verify.__file__)),
    }


def dependency_snapshot(build: Path, context: dict) -> dict | None:
    inputs, objects = {}, {}
    for target in context["commands"]:
        output = build / target
        if not output.is_file():
            return None
        text = subprocess.check_output(
            [context["tools"]["CMAKE_MAKE_PROGRAM"]["path"], "-C", str(build), "-t", "deps", target],
            text=True, timeout=10,
        )
        lines = text.splitlines()
        header = re.fullmatch(re.escape(target) + r": #deps (\d+), deps mtime \d+ \(VALID\)", lines[0]) if lines else None
        dependencies = [line[4:] for line in lines[1:] if line.startswith("    ")]
        if (
            header is None or int(header.group(1)) != len(dependencies)
            or not dependencies or len(set(dependencies)) != len(dependencies)
            or any(line and not line.startswith("    ") for line in lines[1:])
        ):
            return None
        paths = {(build / name).resolve() for name in dependencies}
        if Path(context["sources"][target]) not in paths:
            return None
        for path in sorted(paths):
            if not path.is_file():
                return None
            if str(path) not in inputs:
                inputs[str(path)] = digest(path)
        objects[target] = digest(output)
    return {"dependencies": inputs, "objects": objects}


def reusable_execution(previous: bytes | None, origin: Path, inputs: dict) -> dict | None:
    if previous is None:
        return None
    summary = json.loads(previous)
    if not isinstance(summary, dict) or summary.get("schema_version") != 1:
        raise verify.VerificationError("malformed prior analysis report")
    if summary.get("status") != "passed" or not summary.get("reusable"):
        return None
    if not all(isinstance(summary.get(key), str) for key in ("execution_record", "execution_record_sha256")):
        raise verify.VerificationError("prior analysis report has no execution record")
    record_path = Path(summary["execution_record"]).resolve()
    if not record_path.is_relative_to(origin.parent.resolve()) or record_path.name != "execution.json":
        raise verify.VerificationError("prior analysis evidence escapes its artifact owner")
    if not record_path.is_file() or digest(record_path) != summary["execution_record_sha256"]:
        return None
    record = json.loads(record_path.read_bytes())
    if not isinstance(record, dict) or not all(
        isinstance(record.get(key), str) for key in ("log", "log_sha256", "execution_id")
    ):
        raise verify.VerificationError("malformed original analysis execution record")
    log = Path(record["log"]).resolve()
    if not log.is_relative_to(record_path.parent):
        raise verify.VerificationError("prior analysis log escapes its execution owner")
    if (
        record.get("status") != "passed" or record.get("analysis") != "executed"
        or record.get("inputs") != inputs or record.get("reusable") is not True
        or record.get("executed_targets") != sorted(inputs["context"]["commands"])
        or not log.is_file() or digest(log) != record["log_sha256"]
    ):
        return None
    return record | {
        "execution_record": str(record_path), "execution_record_sha256": digest(record_path),
    }


def run_reported_analysis(
    directory: Path, assigned: list[Path], database: dict[Path, str],
    report: Path, reuse_report: Path | None, jobs: int,
) -> dict:
    verify.require_owned_process_execution()
    build, report = (ROOT / directory).resolve(), (ROOT / report).resolve()
    reuse_report = (ROOT / reuse_report).resolve() if reuse_report is not None else None
    verify.validate_report_path(ROOT, report)
    owner = build / "verification" / "changed-tidy-owner.json"
    verify.validate_report_path(ROOT, owner)
    if report.suffix != ".json" or report in {
        owner, owner.with_name(owner.name + ".lock"), build / "compile_commands.json",
    }:
        raise verify.VerificationError("analysis report conflicts with build inputs or its ownership lock")
    with verify.catch_termination(), verify.evidence_lock(owner), verify.evidence_lock(report) as run_id:
        previous = reuse_report.read_bytes() if reuse_report is not None and reuse_report.is_file() else None
        artifacts = Path(tempfile.mkdtemp(prefix="analysis-", dir=report.parent))
        value = {
            "schema_version": 1, "status": "running", "run_id": run_id,
            "artifacts": str(artifacts),
            "analysis": "not-executed", "executed_targets": [], "reusable": False,
            "qualification_complete": False,
        }
        started = time.monotonic()
        verify.write_json_report(report, value)
        try:
            if not assigned:
                value.update(status="passed", analysis="not-required")
                return value
            context = analysis_context(build, assigned, database)
            before = dependency_snapshot(build, context)
            inputs = {"context": context, "compiled_inputs": before}
            old = reusable_execution(previous, reuse_report, inputs) if (
                previous is not None and context["full_dependency_mode"] and before is not None
            ) else None
            if old is not None:
                if analysis_context(build, assigned, database) != context or dependency_snapshot(build, context) != before:
                    raise verify.VerificationError("analysis reuse inputs changed during reconciliation")
                value.update(
                    status="passed", analysis="reused", reusable=True, inputs=inputs,
                    execution_id=old["execution_id"], execution_record=old["execution_record"],
                    execution_record_sha256=old["execution_record_sha256"],
                )
                return value
            value["reuse_outcome"] = "no matching complete prior execution"
            for target in context["commands"]:
                (build / target).unlink(missing_ok=True)
            log = artifacts / "analysis.log"
            value.update(analysis="running", log=str(log), inputs=inputs)
            verify.write_json_report(report, value)
            with log.open("wb") as output:
                code = verify.run_owned_command(
                    [context["tools"]["cmake"]["path"], "--build", str(build), "--parallel",
                     str(max(1, jobs)), "--verbose", "--target", *sorted(context["commands"])],
                    ROOT, output,
                )
            value["return_code"] = code
            value["log_sha256"] = digest(log)
            if code:
                raise verify.VerificationError(f"analysis build exited with code {code}; see {log}")
            observed = {target: 0 for target in context["commands"]}
            with log.open(encoding="utf-8", errors="replace") as stream:
                for line in stream:
                    for target, command in context["commands"].items():
                        if command in line:
                            observed[target] += 1
            if any(count != 1 for count in observed.values()):
                raise verify.VerificationError("successful build did not execute every selected analyzer command once")
            if any(not (build / target).is_file() for target in observed):
                raise verify.VerificationError("successful analysis did not produce every selected object")
            if analysis_context(build, assigned, database) != context:
                raise verify.VerificationError("analysis source, configuration or tool inputs changed")
            after = dependency_snapshot(build, context)
            if before is not None and after is not None and any(
                after["dependencies"][name] != checksum
                for name, checksum in before["dependencies"].items() if name in after["dependencies"]
            ):
                raise verify.VerificationError("a compiler dependency changed during analysis")
            reusable = (
                context["full_dependency_mode"] and before is not None and after is not None
                and before["dependencies"] == after["dependencies"]
            )
            value.update(
                status="passed", analysis="executed", execution_id=run_id,
                executed_targets=sorted(observed), reusable=bool(reusable),
                inputs={"context": context, "compiled_inputs": after},
                elapsed_seconds=time.monotonic() - started,
            )
            execution = artifacts / "execution.json"
            verify.write_json_report(execution, value)
            value.update(execution_record=str(execution), execution_record_sha256=digest(execution))
            return value
        except KeyboardInterrupt:
            value.update(status="interrupted", analysis="interrupted")
            raise
        except (verify.VerificationError, OSError, ValueError, subprocess.SubprocessError) as error:
            value.update(status="failed", analysis="failed", error=str(error))
            raise
        finally:
            value["elapsed_seconds"] = time.monotonic() - started
            verify.write_json_report(artifacts / "result.json", value)
            verify.write_json_report(report, value)


def main() -> int:
    args = parse_arguments()
    if args.report is not None:
        verify.require_owned_process_execution()
    paths = changed_files(args.base_ref)
    database = compile_commands(args.build_directory)
    sources = {
        (ROOT / path).resolve()
        for path in paths
        if path.suffix in SOURCE_SUFFIXES
        and is_production_source(ROOT / path)
        and (ROOT / path).resolve() in database
    }
    for selected in args.smoke_source or []:
        smoke_source = (ROOT / selected).resolve()
        if not is_production_source(smoke_source):
            print(
                f"Smoke source is outside src/: {selected}",
                file=sys.stderr,
            )
            return 1
        if smoke_source not in database:
            print(
                f"Smoke source is missing from compile_commands.json: {selected}",
                file=sys.stderr,
            )
            return 1
        sources.add(smoke_source)
    assigned = (
        sorted(sources) if args.shard_index is None
        else partition_sources(sources)[args.shard_index]
    )
    targets = sorted(database[source] for source in assigned)
    print("changed-clang-tidy-selection " + json.dumps({
        "schema_version": 1,
        "shard_index": args.shard_index if args.shard_index is not None else 0,
        "shard_count": SHARD_COUNT if args.shard_index is not None else 1,
        "parallel": max(1, args.jobs),
        "selected_sources": [source.relative_to(ROOT).as_posix() for source in sorted(sources)],
        "assigned_sources": [source.relative_to(ROOT).as_posix() for source in assigned],
        "targets": targets,
    }, sort_keys=True), flush=True)
    if args.report is not None:
        result = run_reported_analysis(
            args.build_directory, assigned, database, args.report, args.reuse_report, args.jobs,
        )
        print("changed-clang-tidy-result " + json.dumps({
            "status": result["status"], "analysis": result["analysis"],
            "executed_targets": result["executed_targets"], "reusable": result["reusable"],
            "report": str(args.report.resolve()),
        }), flush=True)
        return 0
    if not assigned:
        print(
            "No changed translation units require clang-tidy." if not sources
            else "This clang-tidy shard has no assigned translation units."
        )
        return 0

    print(f"Building {len(targets)} clang-tidy translation unit target(s).")
    completed = subprocess.run(
        [
            "cmake",
            "--build",
            str(ROOT / args.build_directory),
            "--parallel",
            str(max(1, args.jobs)),
            "--target",
            *targets,
        ],
        cwd=ROOT,
        check=False,
    )
    return completed.returncode


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("analysis interrupted; owned processes retired", file=sys.stderr)
        raise SystemExit(130)
    except (verify.VerificationError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"analysis error: {error}", file=sys.stderr)
        raise SystemExit(1)
