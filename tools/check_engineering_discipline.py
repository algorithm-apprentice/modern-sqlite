#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
BTREE_WRITER_TESTS = (
    ROOT / "tests/unit/storage/btree/writer_internal_test.cpp",
    ROOT / "tests/unit/storage/btree/writer_internal_oom_test.cpp",
)
MARKER = "BYTE_VECTOR_RESIZABLE_SCRATCH"
RAW_BYTE_VECTOR = "std::vector<std::byte>"


def check_byte_ownership(path: Path) -> list[str]:
    if not path.exists():
        return []

    errors: list[str] = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for index, line in enumerate(lines):
        if RAW_BYTE_VECTOR not in line:
            continue
        previous = lines[index - 1] if index > 0 else ""
        if MARKER not in line and MARKER not in previous:
            errors.append(
                f"{path.relative_to(ROOT)}:{index + 1}: "
                "owned raw bytes must use ByteBuffer; "
                f"resize-required scratch needs {MARKER}"
            )
    return errors


def main() -> int:
    errors: list[str] = []
    for path in BTREE_WRITER_TESTS:
        errors.extend(check_byte_ownership(path))

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    if "set(CMAKE_CXX_EXTENSIONS OFF)" not in cmake:
        errors.append("CMakeLists.txt: strict standard C++ extensions must be disabled")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print("Engineering discipline checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
