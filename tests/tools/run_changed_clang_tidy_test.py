#!/usr/bin/env python3

import pathlib
import sys
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import run_changed_clang_tidy


class ProductionSourceScopeTest(unittest.TestCase):
    def test_accepts_src_translation_units(self) -> None:
        self.assertTrue(
            run_changed_clang_tidy.is_production_source(
                REPOSITORY_ROOT / "src/base/bytes.cpp"
            )
        )

    def test_rejects_non_src_translation_units(self) -> None:
        for path in (
            "tests/unit/base/bytes_test.cpp",
            "benchmarks/read_performance.cpp",
            "tools/modern_sqlite_inspect.cpp",
            "src/../tests/unit/base/bytes_test.cpp",
        ):
            with self.subTest(path=path):
                self.assertFalse(
                    run_changed_clang_tidy.is_production_source(
                        REPOSITORY_ROOT / path
                    )
                )


if __name__ == "__main__":
    unittest.main()
