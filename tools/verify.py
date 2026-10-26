#!/usr/bin/env python3

from __future__ import annotations

import argparse
from contextlib import contextmanager
import errno
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
import xml.etree.ElementTree as ET

if os.name == "nt":
    import msvcrt
else:
    import fcntl


ROOT = Path(__file__).resolve().parents[1]
POLICY_PATH = Path("project/verification-policy.json")
ORIGINAL_MODEL = (
    "ReadSessionModel."
    "MatchesCorrelatedRowsGroupsAndNullExtendedSourcesAcrossRebindings"
)


class VerificationError(RuntimeError):
    pass


def windows_evidence_descriptor(path: Path) -> int:
    import ctypes
    from ctypes import wintypes

    class AttributeTagInfo(ctypes.Structure):
        _fields_ = [("attributes", wintypes.DWORD), ("tag", wintypes.DWORD)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.GetFileInformationByHandleEx.argtypes = [
        wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
    ]
    kernel.GetFileInformationByHandleEx.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.CreateFileW(
        str(path), 0x80000000 | 0x40000000, 1 | 2, None, 4,
        0x80 | 0x00200000, None,
    )
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        information = AttributeTagInfo()
        if not kernel.GetFileInformationByHandleEx(
            handle, 9, ctypes.byref(information), ctypes.sizeof(information)
        ):
            raise ctypes.WinError(ctypes.get_last_error())
        if information.attributes & 0x400:
            raise VerificationError("report lock path is a Windows reparse point")
        descriptor = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY | os.O_NOINHERIT)
        handle = None
        try:
            os.set_inheritable(descriptor, False)
        except OSError:
            os.close(descriptor)
            raise
        return descriptor
    finally:
        if handle is not None:
            kernel.CloseHandle(handle)


@contextmanager
def evidence_lock(report: Path):
    report.parent.mkdir(parents=True, exist_ok=True)
    path = report.with_name(report.name + ".lock")
    descriptor = (
        windows_evidence_descriptor(path) if os.name == "nt"
        else os.open(path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    )
    try:
        try:
            if os.name == "nt":
                msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
            else:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if isinstance(error, BlockingIOError) or (
                os.name == "nt" and error.errno in {errno.EACCES, errno.EAGAIN, errno.EDEADLK}
            ):
                raise VerificationError("report path is in use by another invocation") from error
            raise
        yield uuid.uuid4().hex
    finally:
        os.close(descriptor)


def write_json_report(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(value, stream, indent=2)
            stream.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def environment_identity() -> str:
    return hashlib.sha256(json.dumps(dict(os.environ), sort_keys=True).encode()).hexdigest()


def repository_identity(root: Path) -> str:
    paths = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=root
    ).split(b"\0")
    values = []
    for raw in sorted(set(paths) - {b""}):
        relative = raw.decode("utf-8")
        path = root / relative
        if path.is_symlink():
            content = "symlink:" + os.readlink(path)
        elif path.is_file():
            content = hashlib.sha256(path.read_bytes()).hexdigest()
        else:
            content = "deleted"
        values.append((relative, content))
    return hashlib.sha256(json.dumps(values).encode()).hexdigest()


def command_file_identities(commands: list[list[str]]) -> dict[str, str]:
    result = {}
    for command in commands:
        for index, argument in enumerate(command):
            if index and (argument.startswith("-") or len(argument) > 4096):
                continue
            resolved = shutil.which(argument) if index == 0 else argument
            if resolved is not None and Path(resolved).is_file():
                path = str(Path(resolved).resolve())
                if path not in result:
                    result[path] = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    return result


def shared_definitions(directory: Path, policy: dict) -> dict[str, dict]:
    result = subprocess.run(
        ["ctest", "--test-dir", str(directory), "-L", policy["shared_test_label"],
         "--show-only=json-v1"],
        capture_output=True, text=True, check=False,
    )
    if result.returncode:
        raise VerificationError(f"shared discovery failed in {directory}: {result.stderr.strip()}")
    data = json.loads(result.stdout)
    if not isinstance(data, dict) or not isinstance(data.get("tests"), list):
        raise VerificationError("invalid shared CTest discovery")
    definitions = {}
    for test in data["tests"]:
        if not isinstance(test, dict) or test.get("name") not in policy["configuration_independent_tests"]:
            raise VerificationError("unknown shared test selection")
        name = test["name"]
        command = test.get("command")
        if name in definitions or not isinstance(command, list) or not command or any(
            not isinstance(argument, str) for argument in command
        ):
            raise VerificationError("invalid shared test command or duplicate name")
        properties = {}
        for item in test.get("properties", []):
            if not isinstance(item, dict) or "name" not in item or "value" not in item:
                raise VerificationError("invalid shared test properties")
            if item["name"] not in {"LABELS", "DEF_SOURCE_LINE"}:
                properties[item["name"]] = item["value"]
        definitions[name] = {"command": command, "properties": properties}
    if not definitions:
        raise VerificationError(f"empty shared test selection in {directory}")
    return definitions


def run_shared(owner: Path, directories: list[Path], report: Path, root: Path, policy: dict) -> dict:
    owner = owner.resolve()
    directories = list(dict.fromkeys([owner, *[directory.resolve() for directory in directories]]))
    with evidence_lock(report) as run_id:
        value = {"status": "running", "run_id": run_id, "process_mode": "shared-checks"}
        write_json_report(report, value)
        started = time.monotonic()
        try:
            source = repository_identity(root)
            environment = environment_identity()
            definitions = {str(directory): shared_definitions(directory, policy) for directory in directories}
            owning = definitions[str(owner)]
            for directory, tests in definitions.items():
                for name, definition in tests.items():
                    if name not in owning:
                        raise VerificationError(f"shared owner is missing {name} required by {directory}")
                    if owning[name] != definition:
                        raise VerificationError(f"shared identity differs for {name} in {directory}")
            files = command_file_identities([test["command"] for test in owning.values()])
            artifacts = Path(tempfile.mkdtemp(prefix="shared-", dir=report.parent))
            xml = artifacts / "shared.xml"
            with (artifacts / "shared.log").open("w") as log:
                result = subprocess.run(
                    ["ctest", "--test-dir", str(owner), "-L", policy["shared_test_label"],
                     "--parallel", "1", "--output-on-failure", "--no-tests=error",
                     "--output-junit", str(xml)],
                    stdout=log, stderr=subprocess.STDOUT, check=False,
                )
            if result.returncode:
                raise VerificationError("shared CTest execution failed")
            result_xml = ET.parse(xml).getroot()
            actual = [case.attrib.get("name") for case in result_xml.iter("testcase")]
            if (
                len(actual) != len(set(actual))
                or set(actual) != set(owning)
                or any(int(result_xml.attrib.get(key, 0)) for key in ("failures", "errors", "skipped", "disabled"))
            ):
                raise VerificationError("shared execution coverage is incomplete")
            after = {str(directory): shared_definitions(directory, policy) for directory in directories}
            if (
                after != definitions
                or repository_identity(root) != source
                or environment_identity() != environment
                or command_file_identities([test["command"] for test in owning.values()]) != files
            ):
                raise VerificationError("shared input/configuration identity changed during execution")
            value.update({
                "status": "passed", "owner": str(owner),
                "covered_instances": sum(len(tests) for tests in definitions.values()),
                "executed_tests": len(owning),
                "test_names": sorted(owning),
                "configuration_identities": {
                    directory: hashlib.sha256(json.dumps(tests, sort_keys=True).encode()).hexdigest()
                    for directory, tests in definitions.items()
                },
                "source_sha256": source, "environment_sha256": environment,
                "command_files_sha256": files, "artifacts": str(artifacts),
            })
        except (VerificationError, OSError, ValueError, ET.ParseError, subprocess.CalledProcessError) as error:
            value.update({"status": "failed", "error": str(error)})
            raise
        finally:
            value["elapsed_seconds"] = time.monotonic() - started
            write_json_report(report, value)
        return value


def load_policy(root: Path = ROOT) -> dict:
    policy = json.loads((root / POLICY_PATH).read_text(encoding="utf-8"))
    if policy.get("schema_version") != 1:
        raise VerificationError("unsupported verification policy schema")
    for key in ("native_targets", "configuration_independent_tests"):
        values = policy.get(key)
        if not isinstance(values, list) or not values or any(
            not isinstance(value, str) or not value for value in values
        ):
            raise VerificationError(f"policy {key} must be a nonempty string list")
        if len(set(values)) != len(values):
            raise VerificationError(f"policy {key} contains duplicate entries")
    if any(
        re.fullmatch(r"modern_sqlite_[a-z0-9_]+", target) is None
        for target in policy["native_targets"]
    ):
        raise VerificationError("policy contains an invalid native target")
    label = policy.get("shared_test_label")
    if not isinstance(label, str) or re.fullmatch(r"[a-z][a-z0-9-]+", label) is None:
        raise VerificationError("invalid shared-test label")
    fast = policy.get("fast_local", {})
    if not isinstance(fast, dict):
        raise VerificationError("invalid fast-local policy")
    for key, upper in (
        ("maximum_batch_cases", 128),
        ("boundary_seconds", 10),
        ("capture_bytes", 16 * 1024 * 1024),
    ):
        value = fast.get(key)
        if not isinstance(value, int) or isinstance(value, bool) or not 0 < value <= upper:
            raise VerificationError(f"invalid fast-local {key}")
    if fast.get("ordinary_unit_executable") != "modern_sqlite_unit_tests":
        raise VerificationError("only the ordinary unit executable may be batched")
    domains = policy.get("domains")
    if not isinstance(domains, list) or not domains:
        raise VerificationError("verification domains are missing")
    for domain in domains:
        if (
            not isinstance(domain, dict)
            or not isinstance(domain.get("name"), str)
            or not isinstance(domain.get("paths"), list)
            or not domain["paths"]
            or any(not isinstance(path, str) or not path for path in domain["paths"])
        ):
            raise VerificationError("invalid verification domain")
        if domain["name"] not in {
            "documentation", "tooling", "foundation", "storage", "sql",
            "performance", "reference", "metadata", "tests", "durability",
        }:
            raise VerificationError("unknown verification policy domain")
    if len({domain["name"] for domain in domains}) != len(domains):
        raise VerificationError("duplicate verification domain")
    return policy


def normalized_path(value: str) -> str:
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or ".." in path.parts
        or "\\" in value
        or path == PurePosixPath(".")
    ):
        raise VerificationError(f"change path must be inside the repository: {value!r}")
    return str(path)


def git_changed_paths(root: Path, base: str) -> list[str]:
    if not base or base.startswith("-"):
        raise VerificationError("invalid base ref")
    output = subprocess.check_output(
        ["git", "diff", "--name-status", "-z", "--find-renames", base, "--"],
        cwd=root,
    ).split(b"\0")
    paths = []
    index = 0
    while index < len(output) and output[index]:
        status = output[index].decode("ascii")
        count = 2 if status[0] in "RC" else 1
        if index + count >= len(output):
            raise VerificationError("malformed Git change record")
        for offset in range(1, count + 1):
            paths.append(output[index + offset].decode("utf-8"))
        index += count + 1
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=root
    )
    paths.extend(value.decode("utf-8") for value in untracked.split(b"\0") if value)
    return sorted({normalized_path(path) for path in paths})


def changed_domains(paths: list[str], policy: dict) -> list[str]:
    domains = set()
    for path in paths:
        if path.endswith(".md"):
            domains.add("documentation")
            continue
        matching = {
            domain["name"]
            for domain in policy["domains"]
            if any(fnmatch.fnmatchcase(path, pattern) for pattern in domain["paths"])
        }
        domains.update(matching or {"unknown"})
    return sorted(domains)


def make_plan(root: Path, paths: list[str], policy: dict, args: argparse.Namespace) -> dict:
    paths = sorted({normalized_path(path) for path in paths})
    if not paths:
        raise VerificationError("empty change selection; specify a base ref or changed file")
    domains = changed_domains(paths, policy)
    selected = set(domains)
    full = args.profile == "full" or bool(
        selected & {"unknown", "foundation", "reference", "durability"}
    )
    known_python_tests = ("tests/tools/", "tests/project/", "tests/differential/",
                          "tests/cli/", "tests/cmake/", "tests/performance/")
    if any(
        path.startswith("tests/") and not path.startswith(known_python_tests)
        and path != "tests/CMakeLists.txt"
        and Path(path).suffix not in {".cpp", ".cc", ".cxx", ".hpp", ".h", ".md"}
        for path in paths
    ):
        full = True
    known_tools = {
        "tools/verify.py", "tools/run_fast_tests.py", "tools/run_changed_clang_tidy.py",
        "tools/check_engineering_discipline.py", "tools/project_graph.py",
        "tools/read_compatibility.py", "tools/__init__.py",
        "tools/modern_sqlite_inspect.cpp", "tools/modern_sqlite_read_trace.cpp",
    }
    if any(
        path.startswith("tools/") and path not in known_tools
        and not fnmatch.fnmatchcase(path, "tools/*_performance.py")
        for path in paths
    ):
        full = True
    cpp = any(Path(path).suffix in {".cpp", ".cc", ".cxx", ".hpp", ".h"} for path in paths)
    profile = (
        "full" if full
        else "documentation" if selected <= {"documentation", "metadata"}
        else "targeted" if cpp or selected & {"storage", "sql", "performance"}
        else "tooling"
    )
    steps = []
    outstanding = ["documentation-review"]
    if any(path.startswith(("tools/", "tests/")) for path in paths):
        outstanding.append("owned-tests-reviewed-and-executed")
    label = policy["shared_test_label"]

    def add(identifier: str, command: list[str], purpose: str) -> None:
        previous = next((step for step in steps if step["id"] == identifier), None)
        if previous is not None:
            if previous["command"] != command:
                raise VerificationError(f"conflicting verification step: {identifier}")
            return
        steps.append({"id": identifier, "command": command, "purpose": purpose})

    def configure_build(configuration: str) -> None:
        add(
            f"configure-{configuration}", ["cmake", "--preset", configuration],
            "Configure without changing test workloads or limits.",
        )
        add(
            f"build-{configuration}",
            ["cmake", "--build", "--preset", configuration, "--parallel", "2"],
            "Incremental build for this evidence domain.",
        )

    def ctest(identifier: str, directory: str, selectors: list[str], exclusion: str) -> None:
        add(
            identifier,
            ["ctest", "--test-dir", directory, "--output-on-failure", "--no-tests=error",
             *selectors, "-LE", exclusion],
            "Selected runtime coverage; shared checks are owned by one earlier step.",
        )

    add("graph", [sys.executable, "tools/project_graph.py", "validate"], "Graph consistency.")
    if any(path.startswith(("src/", "include/", "tools/")) for path in paths):
        outstanding.append("focused-red-green-or-green-refactor-baseline")
    if "project/manifest.json" in paths:
        outstanding.append("manifest-automation-review")
    if profile == "documentation":
        return plan_result(root, paths, domains, profile, steps, outstanding)
    outstanding.append("nine-exact-head-hosted-checks")
    add(
        "engineering", [sys.executable, "tools/check_engineering_discipline.py"],
        "Repository engineering invariants.",
    )
    if profile == "tooling":
        add(
            "tool-tests",
            [sys.executable, "-m", "unittest", "discover", "-s", "tests/tools", "-p", "*test.py"],
            "Tool, negative-control, workflow, and supervision contracts.",
        )
        if any(path.startswith(("tests/project/", "project/")) or path == "tools/project_graph.py"
               for path in paths):
            add("project-tests", [sys.executable, "-m", "unittest", "discover",
                                  "-s", "tests/project", "-p", "test_*.py"],
                "Execute the project/metadata owner tests, not only graph validation.")
        differential = any(path.startswith("tests/differential/") or path == "tools/read_compatibility.py"
                           for path in paths)
        cli = any(path.startswith("tests/cli/") for path in paths)
        cmake = any(path.startswith("tests/cmake/") or path in {
            "tests/CMakeLists.txt", "cmake/Verification.cmake"
        } for path in paths)
        if differential or cli or cmake:
            add("configure-dev-debug", ["cmake", "--preset", "dev-debug"],
                "Configure the affected harness integration entry points.")
        if differential or cli:
            targets = ["modern_sqlite_read_trace"]
            if cli:
                targets.append("modern_sqlite_inspect")
            add("build-harness-runners", ["cmake", "--build", "--preset", "dev-debug",
                                          "--parallel", "2", "--target", *targets],
                "Build only the actual harness executables.")
        for enabled, owner in ((differential, "differential"), (cli, "cli"), (cmake, "cmake")):
            if enabled:
                add("owned-" + owner,
                    ["ctest", "--test-dir", "build/dev-debug", "-L", owner,
                     "--output-on-failure", "--no-tests=error"],
                    "Run the changed harness/integration owner.")
        if any("oracle" in path and path.startswith("tests/differential/") for path in paths):
            add("owned-oracle", ["ctest", "--test-dir", "build/dev-debug",
                                 "-R", "^differential\\..*oracle.*$",
                                 "--output-on-failure", "--no-tests=error"],
                "Require the actual configured pinned-oracle owner selection.")
            outstanding.append("configured-native-oracle-controls")
        outstanding.append("configured-ctest-discovery-reconciliation")
        return plan_result(root, paths, domains, profile, steps, outstanding)

    configure_build("dev-debug")
    add(
        "shared-checks",
        [sys.executable, "tools/verify.py", "run-shared",
         "--owner-build-directory", "build/dev-debug",
         "--report", "build/dev-debug/verification/shared.json"],
        "Run configuration-independent checks exactly once in this local plan.",
    )
    if full:
        ctest("isolated-dev-debug", "build/dev-debug", [], label)
    else:
        add(
            "fast-local",
            [sys.executable, "tools/run_fast_tests.py", "--build-directory", "build/dev-debug",
             "--report", "build/dev-debug/verification/fast-local.json"],
            "Complete fast unit selection, explicitly distinct from isolated qualification.",
        )
        ctest("development-remainder", "build/dev-debug", [], f"unit|{label}")

    outstanding.append("isolated-sanitizer-original-model")
    if full:
        for configuration in ("release", "asan-ubsan", "instrumentation"):
            configure_build(configuration)
            ctest(f"isolated-{configuration}", f"build/{configuration}", [], label)
    else:
        affected = (
            "(Record|Btree|Page|Pager|Journal|Transaction|Freelist|PointerMap|Cache)"
            if "storage" in selected
            else "(Binder|Bound|Lexer|Parser|Syntax|Logical|Physical|Optimizer|Lowering|"
                 "Vm|VM|ReadSession|Sql|Function|Aggregate|Collation)"
        )
        configure_build("asan-ubsan")
        ctest("affected-asan", "build/asan-ubsan", ["-R", affected + "|" + ORIGINAL_MODEL], label)
        if any(
            path.startswith(("src/optimizer/", "src/lowering/", "src/vm/", "src/session/"))
            for path in paths
        ):
            configure_build("instrumentation")
            ctest(
                "fixed-source-work", "build/instrumentation",
                ["-R", "^ReadSession.RecordsSourceCorrelatedScalarAndExistsFixedWorkBaseline$"],
                label,
            )
            outstanding.append("matched-fixed-work-properties")

    for attribute, identifier in (
        ("gnu_debug_directory", "gnu-debug"),
        ("gnu_release_directory", "gnu-release"),
    ):
        directory = getattr(args, attribute)
        if directory is None:
            if identifier == "gnu-debug" or full:
                outstanding.append(f"configured-{identifier}-validation")
            continue
        if identifier == "gnu-release" and not full:
            continue
        add(
            f"build-{identifier}", ["cmake", "--build", directory, "--parallel", "2"],
            "Use an explicitly configured genuine GNU build.",
        )
        outstanding.append("genuine-gnu-compiler-identity")
        ctest(
            f"isolated-{identifier}" if full else "affected-gnu",
            directory, [] if full else ["-R", affected], label,
        )

    campaigns = []
    if full or any(path.startswith("src/syntax/") for path in paths):
        campaigns.append(("sql", "modern_sqlite_read_sql_fuzz", "4096", "read_sql"))
    if full or "storage" in selected:
        campaigns.append(("database", "modern_sqlite_read_database_fuzz", "65536", "read_database"))
    if full:
        campaigns.extend([
            ("write-sql", "modern_sqlite_write_sql_fuzz", "4096", "write_sql"),
            ("write-database", "modern_sqlite_write_database_fuzz", "65536", "write_database"),
        ])
    if campaigns:
        add(
            "configure-fuzz", ["cmake", "--preset", "fuzz"],
            "Configure the bounded sanitizer/fuzzer build before using it.",
        )
        add(
            "build-fuzz",
            ["cmake", "--build", "--preset", "fuzz", "--parallel", "2", "--target",
             *[campaign[1] for campaign in campaigns]],
            "Only affected bounded fuzz targets.",
        )
    for domain, executable, maximum, seeds in campaigns:
        directory = f"build/verification-inputs/{domain}"
        diagnostics = f"build/verification-diagnostics/{domain}"
        add(
            f"prepare-{domain}-inputs",
            ["cmake", "-E", "make_directory", directory, diagnostics],
            "Mutations are written outside repository seed directories.",
        )
        if domain.endswith("database"):
            for name, source in (
                ("core", "tests/fixtures/read_compatibility/sqlite-3.54.0-read-compatibility.db"),
                ("btree", "tests/fixtures/btree_read/sqlite-3.54.0-btree-read.db"),
            ):
                add(
                    f"seed-{domain}-{name}",
                    ["cmake", "-E", "copy_if_different", source, directory],
                    "Copy unchanged valid native images into the writable campaign corpus.",
                )
        add(
            f"fuzz-{domain}",
            [f"build/fuzz/{executable}", "-runs=20000", "-seed=102",
             f"-max_len={maximum}", "-timeout=10", f"-artifact_prefix={diagnostics}/",
             directory, f"tests/fuzz/corpus/{seeds}"],
            "Bounded campaign with unchanged workload/timeout semantics.",
        )
    if full or selected & {"storage", "performance"} or any(
        path.startswith(("src/optimizer/", "src/lowering/", "src/vm/")) for path in paths
    ):
        add(
            "native-build",
            [sys.executable, "tools/verify.py", "build-native",
             "--build-directory", "build/benchmark", "--parallel", "2"],
            "Build only the canonical native contract targets.",
        )
        ctest("native-contracts", "build/benchmark", ["-L", "performance|compatibility"], label)
        outstanding.append("configured-pinned-native-build")
    add(
        "configure-quality", ["cmake", "--preset", "clang-tidy"],
        "Configure existing pinned quality tools without installing dependencies.",
    )
    if full:
        add(
            "quality-all", ["cmake", "--build", "--preset", "clang-tidy",
                            "--parallel", "2", "--target", "modern_sqlite",
                            "modern_sqlite_instrumentation"],
            "Full qualification includes all production-dependent translation units.",
        )
    for path in paths:
        if not full and path.startswith("src/") and Path(path).suffix in {".cpp", ".cc", ".cxx"}:
            add(
                "tidy-" + path.replace("/", "-"),
                [sys.executable, "tools/run_changed_clang_tidy.py",
                 "--base-ref", args.base_ref or "HEAD", "--build-directory", "build/clang-tidy",
                 "--jobs", "2", "--smoke-source", path],
                "Force the actual changed TU even before a commit exists.",
            )
    add(
        "format", ["cmake", "--build", "--preset", "clang-tidy", "--target", "check-format"],
        "Pinned repository formatter.",
    )
    return plan_result(root, paths, domains, profile, steps, outstanding)


def plan_result(root: Path, paths: list[str], domains: list[str], profile: str,
                steps: list[dict], outstanding: list[str]) -> dict:
    shared = next((step for step in steps if step["id"] == "shared-checks"), None)
    if shared is not None:
        directories = sorted({
            step["command"][step["command"].index("--test-dir") + 1]
            for step in steps if step["command"][0] == "ctest"
        } - {"build/dev-debug"})
        for directory in directories:
            shared["command"].extend(["--build-directory", directory])
        prefix = [step for step in steps if step["id"] in {"graph", "engineering"}]
        configure = [step for step in steps if step["id"].startswith("configure-")]
        builds = [step for step in steps if step["id"].startswith("build-")]
        prior_ids = {step["id"] for step in prefix + configure + builds + [shared]}
        steps = prefix + configure + builds + [shared] + [
            step for step in steps if step["id"] not in prior_ids
        ]
    return {
        "schema_version": 1,
        "status": "plan",
        "profile": profile,
        "changes": paths,
        "domains": domains,
        "steps": steps,
        "outstanding_requirements": sorted(set(outstanding)),
        "policy_sha256": hashlib.sha256((root / POLICY_PATH).read_bytes()).hexdigest(),
        "is_execution_evidence": False,
    }


def read_cache(directory: Path) -> dict[str, str]:
    path = directory / "CMakeCache.txt"
    if not path.is_file():
        raise VerificationError(f"build directory is not configured: {directory}")
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith(("#", "//")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    return values


def build_native(directory: Path, parallel: int, policy: dict, root: Path) -> int:
    if parallel < 1:
        raise VerificationError("parallel job count must be positive")
    cache = read_cache(directory)
    if "CMAKE_HOME_DIRECTORY" not in cache or Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != root:
        raise VerificationError("native build directory belongs to a different source checkout")
    if cache.get("CMAKE_BUILD_TYPE") != "Release" or cache.get("MODERN_SQLITE_BUILD_BENCHMARKS") != "ON":
        raise VerificationError("native timing requires a configured Release benchmark build")
    for key in (
        "MODERN_SQLITE_ENABLE_ASAN", "MODERN_SQLITE_ENABLE_UBSAN",
        "MODERN_SQLITE_ENABLE_TSAN", "MODERN_SQLITE_ENABLE_INSTRUMENTATION",
    ):
        if cache.get(key, "OFF") not in {"OFF", "FALSE", "0", ""}:
            raise VerificationError(f"native timing must not enable sanitizer/instrumentation: {key}")
    command = [
        "cmake", "--build", str(directory), "--parallel", str(parallel),
        "--target", *policy["native_targets"],
    ]
    return subprocess.run(command, cwd=root, check=False).returncode


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plan change-aware verification without hiding missing coverage.")
    commands = parser.add_subparsers(dest="operation", required=True)
    plan = commands.add_parser("plan")
    plan.add_argument("--repository-root", type=Path, default=ROOT)
    changes = plan.add_mutually_exclusive_group(required=True)
    changes.add_argument("--base-ref")
    changes.add_argument("--changed-file", action="append")
    plan.add_argument("--profile", choices=("auto", "full"), default="auto")
    plan.add_argument("--gnu-debug-directory")
    plan.add_argument("--gnu-release-directory")
    plan.add_argument("--format", choices=("text", "json"), default="text")
    targets = commands.add_parser("native-targets")
    targets.add_argument("--repository-root", type=Path, default=ROOT)
    targets.add_argument("--format", choices=("text", "json"), default="text")
    native = commands.add_parser("build-native")
    native.add_argument("--repository-root", type=Path, default=ROOT)
    native.add_argument("--build-directory", type=Path, required=True)
    native.add_argument("--parallel", type=int, default=2)
    shared = commands.add_parser("run-shared")
    shared.add_argument("--repository-root", type=Path, default=ROOT)
    shared.add_argument("--owner-build-directory", type=Path, required=True)
    shared.add_argument("--build-directory", type=Path, action="append", default=[])
    shared.add_argument("--report", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.repository_root.resolve()
    try:
        policy = load_policy(root)
        if args.operation == "build-native":
            return build_native(args.build_directory.resolve(), args.parallel, policy, root)
        if args.operation == "run-shared":
            result = run_shared(args.owner_build_directory, args.build_directory,
                                args.report.resolve(), root, policy)
            print(json.dumps({"status": result["status"], "executed_tests": result["executed_tests"],
                              "covered_instances": result["covered_instances"]}))
            return 0
        if args.operation == "native-targets":
            value = policy["native_targets"]
        else:
            paths = args.changed_file or git_changed_paths(root, args.base_ref)
            value = make_plan(root, paths, policy, args)
        if args.format == "json":
            print(json.dumps(value, indent=2))
        elif isinstance(value, list):
            print("\n".join(value))
        else:
            print(f"Verification plan ({value['profile']}); not execution evidence.")
            for step in value["steps"]:
                print(f"{step['id']}: " + json.dumps(step["command"]))
            print("Outstanding: " + ", ".join(value["outstanding_requirements"]))
        return 0
    except (VerificationError, OSError, ValueError, ET.ParseError, subprocess.CalledProcessError) as error:
        print(f"verification error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
