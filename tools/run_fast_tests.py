#!/usr/bin/env python3

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.verify import (
    VerificationError,
    command_file_identities,
    environment_identity,
    evidence_lock,
    load_policy,
    repository_identity,
    write_json_report,
)


START = re.compile(r"^\[ RUN      \] (.+)$")
END = re.compile(r"^\[\s*(OK|FAILED|SKIPPED)\s*\] ([^ ]+) \(([\d.]+) ms\)$")
EXACT_FILTER = re.compile(r"[A-Za-z0-9_./]+")
DEFAULT_SKIP = [r"\[  SKIPPED \]"]
KNOWN_PROPERTIES = {
    "LABELS", "TIMEOUT", "WORKING_DIRECTORY", "DEF_SOURCE_LINE",
    "SKIP_REGULAR_EXPRESSION", "COST",
}


def discover(directory: Path) -> list[dict]:
    completed = subprocess.run(
        ["ctest", "--test-dir", str(directory), "-L", "unit", "--show-only=json-v1"],
        capture_output=True, text=True, check=False,
    )
    if completed.returncode:
        raise VerificationError("CTest unit discovery failed: " + completed.stderr.strip())
    value = json.loads(completed.stdout)
    if not isinstance(value, dict):
        raise VerificationError("invalid CTest discovery object")
    tests = value.get("tests")
    if not isinstance(tests, list) or not tests:
        raise VerificationError("empty unit selection")
    if any(not isinstance(test, dict) for test in tests):
        raise VerificationError("invalid CTest test definition")
    names = [test.get("name") for test in tests]
    if any(not isinstance(name, str) or not name for name in names) or len(set(names)) != len(names):
        raise VerificationError("invalid or duplicate discovered unit names")
    for test in tests:
        if (
            not isinstance(test.get("command"), list) or not test["command"]
            or any(not isinstance(argument, str) for argument in test["command"])
        ):
            raise VerificationError(f"unit test has no command: {test['name']}")
        properties = test.get("properties", [])
        if not isinstance(properties, list) or any(
            not isinstance(item, dict) or not isinstance(item.get("name"), str)
            or "value" not in item for item in properties
        ):
            raise VerificationError("invalid CTest test properties")
        test["property_map"] = {item["name"]: item["value"] for item in properties}
        if len(test["property_map"]) != len(properties):
            raise VerificationError("duplicate CTest test properties")
        if test["property_map"].get("DISABLED"):
            raise VerificationError(f"disabled unit test cannot count as passed: {test['name']}")
    return tests


def ordinary_case(test: dict, executable: str) -> dict | None:
    command = test["command"]
    properties = test["property_map"]
    if Path(command[0]).stem != executable or set(properties) - KNOWN_PROPERTIES:
        return None
    if properties.get("SKIP_REGULAR_EXPRESSION", DEFAULT_SKIP) != DEFAULT_SKIP:
        return None
    filters = [arg.split("=", 1)[1] for arg in command[1:] if arg.startswith("--gtest_filter=")]
    common = [arg for arg in command[1:] if not arg.startswith("--gtest_filter=")]
    if len(filters) != 1 or EXACT_FILTER.fullmatch(filters[0]) is None:
        return None
    if any(arg != "--gtest_also_run_disabled_tests" for arg in common):
        return None
    timeout = properties.get("TIMEOUT")
    if not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or timeout <= 0:
        return None
    return {
        "name": test["name"],
        "filter": filters[0],
        "binary": command[0],
        "common_arguments": common,
        "directory": properties.get("WORKING_DIRECTORY", str(ROOT)),
        "timeout": float(timeout),
    }


def partition(tests: list[dict], policy: dict) -> tuple[list[list[dict]], list[str]]:
    groups = collections.defaultdict(list)
    isolated = []
    for test in tests:
        case = ordinary_case(test, policy["ordinary_unit_executable"])
        if case is None:
            isolated.append(test["name"])
            continue
        key = (case["binary"], tuple(case["common_arguments"]), case["directory"])
        groups[key].append(case)
    batches = []
    maximum = policy["maximum_batch_cases"]
    for cases in groups.values():
        current = []
        length = 0
        filters = set()
        for case in cases:
            if case["filter"] in filters:
                raise VerificationError("duplicate ordinary GoogleTest filter")
            filters.add(case["filter"])
            if len(case["filter"]) > 32768:
                raise VerificationError("ordinary GoogleTest name exceeds the batch argument bound")
            if current and (len(current) >= maximum or length + len(case["filter"]) + 1 > 32768):
                batches.append(current)
                current = []
                length = 0
            current.append(case)
            length += len(case["filter"]) + 1
        if current:
            batches.append(current)
    return batches, isolated


def owned_group_states(leader: int) -> dict[int, str]:
    try:
        output = subprocess.check_output(
            ["ps", "-axo", "pid=,pgid=,stat="], text=True
        )
    except subprocess.CalledProcessError as error:
        raise VerificationError("cannot inspect the owned process group") from error
    result = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) != 3:
            raise VerificationError("invalid process-group inspection result")
        if int(fields[1]) == leader:
            result[int(fields[0])] = fields[2]
    return result


def kill_owned_group(process: subprocess.Popen) -> None:
    # Reap only after group termination, so the owned leader PID cannot be reused.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except PermissionError:
        # Darwin reports EPERM for an exited group containing only zombies.
        states = owned_group_states(process.pid)
        if process.pid not in states or any(not state.startswith("Z") for state in states.values()):
            raise VerificationError("cannot terminate a live owned process group")
    process.wait()


def wait_without_reaping(process: subprocess.Popen, deadline: float) -> None:
    while time.monotonic() < deadline:
        state = owned_group_states(process.pid).get(process.pid)
        if state is None:
            raise VerificationError("owned process status disappeared before cleanup")
        if state.startswith("Z") and time.monotonic() <= deadline:
            return
        time.sleep(0.01)
    raise VerificationError("GoogleTest shutdown timed out")


def xml_cases(path: Path, expected: set[str], google_test: bool) -> set[str]:
    if not path.is_file():
        raise VerificationError("missing XML test report")
    root = ET.parse(path).getroot()
    try:
        for attribute in ("failures", "errors", "skipped", "disabled"):
            if int(root.attrib.get(attribute, "0")):
                raise VerificationError(f"XML reports {attribute}")
        names = []
        for case in root.iter("testcase"):
            if (
                any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))
                or case.attrib.get("status") == "notrun"
            ):
                raise VerificationError("XML reports a failed, skipped, or unexecuted case")
            name = case.attrib["name"]
            if google_test:
                name = case.attrib["classname"] + "." + name
            names.append(name)
        count = int(root.attrib.get("tests", "-1"))
    except (KeyError, TypeError, ValueError) as error:
        raise VerificationError("invalid XML test report schema") from error
    if len(names) != len(set(names)) or set(names) != expected:
        raise VerificationError("XML case accounting differs from the exact selection")
    if count != len(expected):
        raise VerificationError("XML test count differs from the exact selection")
    return set(names)


def run_batch(cases: list[dict], output: Path, index: int, policy: dict, note=None) -> set[str]:
    if not hasattr(os, "killpg"):
        raise VerificationError("fast-local batches require owned POSIX process groups")
    by_filter = {case["filter"]: case for case in cases}
    expected = set(by_filter)
    report = output / f"batch-{index}.xml"
    command = [
        cases[0]["binary"], *cases[0]["common_arguments"],
        "--gtest_color=no", "--gtest_filter=" + ":".join(by_filter),
        "--gtest_output=xml:" + str(report),
    ]
    active = None
    completed = set()
    boundary = policy["boundary_seconds"]
    deadline = time.monotonic() + boundary
    pending = bytearray()
    captured = 0

    def event(line: bytes) -> None:
        nonlocal active, deadline
        text = line.decode("utf-8", errors="replace").rstrip("\r")
        start = START.fullmatch(text)
        end = END.fullmatch(text)
        if start:
            name = start[1]
            if active is not None or name not in expected or name in completed:
                raise VerificationError("invalid or duplicate GoogleTest start event")
            active = name
            if note is not None:
                note(by_filter[name]["name"], "running")
            deadline = time.monotonic() + by_filter[name]["timeout"]
        elif end:
            state, name = end[1], end[2]
            if active != name:
                raise VerificationError("GoogleTest end event has no matching start")
            if time.monotonic() > deadline or float(end[3]) > by_filter[name]["timeout"] * 1000:
                if note is not None:
                    note(by_filter[name]["name"], "timed_out")
                raise VerificationError(f"GoogleTest case timed out: {name}")
            if note is not None:
                note(by_filter[name]["name"], {"OK": "passed", "FAILED": "failed",
                                               "SKIPPED": "skipped"}[state])
            if state == "SKIPPED":
                raise VerificationError(f"skipped GoogleTest case: {name}")
            if state == "FAILED":
                raise VerificationError(f"failed GoogleTest case: {name}")
            completed.add(name)
            active = None
            deadline = time.monotonic() + boundary

    process = subprocess.Popen(
        command, cwd=cases[0]["directory"], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, start_new_session=True,
    )
    selector = None
    assert process.stdout is not None and process.stderr is not None
    try:
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ, "stdout")
        selector.register(process.stderr, selectors.EVENT_READ, "stderr")
        with (output / f"batch-{index}.log").open("wb") as log:
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise VerificationError(f"GoogleTest case or boundary timed out: {active or 'boundary'}")
                events = selector.select(remaining)
                if not events:
                    raise VerificationError(f"GoogleTest case or boundary timed out: {active or 'boundary'}")
                for key, _ in events:
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    captured += len(chunk)
                    if captured > policy["capture_bytes"]:
                        raise VerificationError("batch output exceeded the capture limit")
                    log.write(chunk)
                    if key.data == "stdout":
                        pending.extend(chunk)
                        while b"\n" in pending:
                            line, _, tail = pending.partition(b"\n")
                            pending[:] = tail
                            event(bytes(line))
            if pending:
                event(bytes(pending))
        wait_without_reaping(process, deadline)
        kill_owned_group(process)
    except VerificationError as error:
        if active is not None and note is not None and "timed out" in str(error):
            note(by_filter[active]["name"], "timed_out")
        raise
    finally:
        if selector is not None:
            selector.close()
        process.stdout.close()
        process.stderr.close()
        if process.returncode is None:
            kill_owned_group(process)
        if active is not None and note is not None:
            note(by_filter[active]["name"], "interrupted")
    if process.returncode != 0:
        raise VerificationError(f"GoogleTest batch exited with code {process.returncode}")
    if active is not None or completed != expected:
        raise VerificationError("GoogleTest event accounting is incomplete")
    xml_cases(report, expected, True)
    return {case["name"] for case in cases}


def run_isolated(directory: Path, names: list[str], output: Path, note=None) -> set[str]:
    if not names:
        return set()
    expression = "^(" + "|".join(re.escape(name) for name in names) + ")$"
    report = output / "isolated.xml"
    command = [
        "ctest", "--test-dir", str(directory), "-L", "unit", "-R", expression,
        "--parallel", "1", "--output-on-failure", "--no-tests=error",
        "--output-junit", str(report),
    ]
    with (output / "isolated.log").open("w") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
    if report.is_file() and note is not None:
        for case in ET.parse(report).iter("testcase"):
            name = case.attrib.get("name")
            if name in names:
                state = "passed"
                for tag in ("failure", "error", "skipped"):
                    if case.find(tag) is not None:
                        state = "skipped" if tag == "skipped" else "failed"
                note(name, state)
    if result.returncode:
        raise VerificationError(f"isolated CTest unit selection exited with code {result.returncode}")
    return xml_cases(report, set(names), False)


def normalized_discovery(tests: list[dict]) -> list[dict]:
    result = []
    for test in tests:
        properties = dict(test["property_map"])
        for name in ("ENVIRONMENT", "ENVIRONMENT_MODIFICATION"):
            if name in properties:
                properties[name] = {"sha256": hashlib.sha256(
                    json.dumps(properties[name], sort_keys=True).encode()
                ).hexdigest()}
        result.append({"name": test["name"], "command": test["command"], "properties": properties})
    return sorted(result, key=lambda item: item["name"])


def execute(directory: Path, report: Path, run_id: str) -> dict:
    started = time.monotonic()
    completed = set()
    outcomes = {}
    value = {
        "schema_version": 1,
        "status": "running",
        "run_id": run_id,
        "process_mode": "fast-local",
        "isolated_qualification": False,
    }
    write_json_report(report, value)

    def note(name: str, state: str) -> None:
        if outcomes.get(name) in {"passed", "failed", "skipped", "timed_out"}:
            return
        outcomes[name] = state
        if state == "passed":
            completed.add(name)

    try:
        policy = load_policy()["fast_local"]
        tests = discover(directory)
        batches, isolated = partition(tests, policy)
        expected = {test["name"] for test in tests}
        normalized = normalized_discovery(tests)
        commands = [test["command"] for test in tests] + [["ctest"]]
        files = command_file_identities(commands)
        source = repository_identity(ROOT)
        environment = environment_identity()
        config_path = directory / "CMakeCache.txt"
        if not config_path.is_file():
            config_path = directory / "CTestTestfile.cmake"
        configuration = hashlib.sha256(config_path.read_bytes()).hexdigest()
        output = Path(tempfile.mkdtemp(prefix=f"fast-local-{run_id[:8]}-", dir=report.parent))
        value.update({
            "test_names": sorted(expected), "normalized_discovery": normalized,
            "batched_tests": sum(len(batch) for batch in batches),
            "isolated_tests": len(isolated), "batch_count": len(batches),
            "discovery_sha256": hashlib.sha256(json.dumps(normalized, sort_keys=True).encode()).hexdigest(),
            "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "policy_sha256": hashlib.sha256(
                (ROOT / "project/verification-policy.json").read_bytes()
            ).hexdigest(),
            "binary_sha256": files, "source_sha256": source,
            "environment_sha256": environment, "configuration_sha256": configuration,
            "artifacts": str(output), "per_case_process_isolation": not batches,
        })
        for index, batch in enumerate(batches):
            run_batch(batch, output, index, policy, note)
        run_isolated(directory, isolated, output, note)
        if completed != expected:
            raise VerificationError("complete unit coverage union differs from discovery")
        if (
            command_file_identities(commands) != files
            or repository_identity(ROOT) != source
            or environment_identity() != environment
            or hashlib.sha256(config_path.read_bytes()).hexdigest() != configuration
            or normalized_discovery(discover(directory)) != normalized
        ):
            raise VerificationError("test input/binary/configuration identity changed during execution")
        value["status"] = "passed"
        return value
    except (VerificationError, OSError, ValueError, ET.ParseError, KeyError, TypeError, AttributeError) as error:
        value["status"] = "failed"
        value["error"] = str(error)
        raise
    finally:
        value["completed_test_names"] = sorted(completed)
        value["started_test_names"] = sorted(outcomes)
        value["case_outcomes"] = outcomes
        value["elapsed_seconds"] = time.monotonic() - started
        write_json_report(report, value)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the complete fast-local unit layer, not isolated qualification."
    )
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    directory = args.build_directory.resolve()
    report = (args.report or directory / "verification/fast-local.json").resolve()
    try:
        with evidence_lock(report) as run_id:
            result = execute(directory, report, run_id)
        print(json.dumps({
            key: result[key]
            for key in ("status", "process_mode", "batched_tests", "isolated_tests",
                        "batch_count", "elapsed_seconds", "isolated_qualification")
        }))
        return 0
    except (VerificationError, OSError, ValueError, ET.ParseError, KeyError, TypeError, AttributeError) as error:
        print(f"fast-local error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
