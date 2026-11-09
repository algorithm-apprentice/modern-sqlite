#!/usr/bin/env python3

import contextlib
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


VERSION = "4.4.5"
ARCHIVE_NAME = f"cmake-{VERSION}-macos-universal.tar.gz"
ARCHIVE_URL = f"https://github.com/Kitware/CMake/releases/download/v{VERSION}/{ARCHIVE_NAME}"
ARCHIVE_SIZE = 89451542
ARCHIVE_SHA256 = "679e77a96b9ef9484cc6c1445255fc4f84206b48a65420efbedf16bdab55fbff"


def _open_archive_response():
    started = time.monotonic()
    try:
        return urllib.request.urlopen(ARCHIVE_URL, timeout=60)
    except urllib.error.HTTPError as error:
        error.close()
        if error.code != 504:
            raise
        remaining = 60 - (time.monotonic() - started)
        if remaining <= 0:
            raise
        print(
            f"CI CMake download returned HTTP504; retrying opening once with {remaining:.3f}s remaining",
            file=sys.stderr,
        )
    try:
        return urllib.request.urlopen(ARCHIVE_URL, timeout=remaining)
    except urllib.error.HTTPError as error:
        error.close()
        raise


def install_profile(runner_root: pathlib.Path, github_path: pathlib.Path) -> pathlib.Path:
    for path in (runner_root, github_path):
        if not path.is_absolute() or "\n" in str(path) or "\r" in str(path):
            raise ValueError(f"CI CMake requires an absolute single-line path: {path}")
    runner_root = runner_root.resolve(strict=True)
    github_path = github_path.resolve(strict=True)
    if not runner_root.is_dir() or not github_path.is_file():
        raise ValueError("CI CMake requires an existing temporary directory and PATH command file")

    with contextlib.ExitStack() as owner:
        prefix = pathlib.Path(tempfile.mkdtemp(prefix="modern-sqlite-cmake-", dir=runner_root))
        owner.callback(shutil.rmtree, prefix)
        archive = prefix / ARCHIVE_NAME
        size = 0
        digest = hashlib.sha256()
        with _open_archive_response() as response, archive.open("xb") as output:
            while chunk := response.read(1024 * 1024):
                size += len(chunk)
                if size > ARCHIVE_SIZE:
                    raise RuntimeError("CI CMake archive exceeds its pinned size")
                if output.write(chunk) != len(chunk):
                    raise OSError("CI CMake archive write was incomplete")
                digest.update(chunk)
        if size != ARCHIVE_SIZE or digest.hexdigest() != ARCHIVE_SHA256:
            raise RuntimeError("CI CMake archive does not match its pinned size and SHA256")

        subprocess.run(
            ["tar", "-xzf", str(archive), "-C", str(prefix)],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        binary = prefix / f"cmake-{VERSION}-macos-universal" / "CMake.app" / "Contents" / "bin"
        tools = (binary / "cmake", binary / "ctest")
        if any(not tool.is_file() for tool in tools):
            raise RuntimeError("CI CMake archive is missing the required executable pair")
        for tool in tools:
            output = subprocess.check_output([str(tool), "--version"], text=True, timeout=10)
            lines = output.splitlines()
            if not lines or lines[0] != f"{tool.name} version {VERSION}":
                raise RuntimeError(f"CI CMake executable has an unexpected version: {tool}")
        archive.unlink()
        line = str(binary) + "\n"
        with github_path.open("a", encoding="utf-8") as output:
            if output.write(line) != len(line):
                raise OSError("CI CMake PATH publication was incomplete")
        owner.pop_all()
        return binary


def main() -> int:
    try:
        if sys.platform != "darwin":
            raise ValueError("CI CMake installation requires macOS")
        runner_root = os.environ.get("RUNNER_TEMP")
        github_path = os.environ.get("GITHUB_PATH")
        if not runner_root or not github_path:
            raise ValueError("CI CMake installation requires RUNNER_TEMP and GITHUB_PATH")
        binary = install_profile(pathlib.Path(runner_root), pathlib.Path(github_path))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"CI CMake setup failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        return 1
    print(json.dumps({
        "version": VERSION, "binary_directory": str(binary),
        "archive_sha256": ARCHIVE_SHA256,
    }), file=sys.stderr)
    print(binary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
