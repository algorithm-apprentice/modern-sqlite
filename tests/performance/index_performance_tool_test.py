#!/usr/bin/env python3

import copy
import pathlib
import sys
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import index_performance
from tools import read_performance


class IndexPerformanceValidationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = REPOSITORY_ROOT
        cls.manifest_path = (
            cls.root / "tests/performance/index-workloads-v1.json"
        )
        cls.manifest = read_performance.load_json_strict(cls.manifest_path)

    def validate(self, value: object) -> dict[str, object]:
        return index_performance.validate_workload_manifest(
            value,
            repository_root=self.root,
            manifest_path=self.manifest_path,
        )

    def test_canonical_manifest_is_valid(self) -> None:
        validated = self.validate(copy.deepcopy(self.manifest))
        self.assertEqual(
            (
                "index-equality-covering-hit",
                "index-equality-covering-miss",
                "index-equality-noncovering-hit",
                "index-multi-equality-covering",
                "index-range-covering",
                "index-range-noncovering",
                "index-range-lower-only-covering",
                "index-range-upper-only-covering",
                "index-unselective-noncovering",
                "index-unselective-covering",
                "index-insert",
                "index-update",
                "index-delete",
                "index-create",
                "index-analyze",
            ),
            tuple(case["id"] for case in validated["cases"]),
        )
        self.assertEqual(
            index_performance.MINIMUM_WALL_NS,
            validated["minimum_wall_ns"],
        )

    def test_rejects_semantic_and_fixture_drift(self) -> None:
        cases = (
            ("minimum_wall_ns", 1, "minimum wall"),
            ("cases", [], "case IDs"),
        )
        for key, replacement, message in cases:
            with self.subTest(key=key):
                value = copy.deepcopy(self.manifest)
                value[key] = replacement
                with self.assertRaisesRegex(
                    index_performance.HarnessError,
                    message,
                ):
                    self.validate(value)

        value = copy.deepcopy(self.manifest)
        value["fixtures"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(
            index_performance.HarnessError,
            "fixture metadata",
        ):
            self.validate(value)

        value = copy.deepcopy(self.manifest)
        value["cases"][0]["measured_iterations"] += 1
        with self.assertRaisesRegex(
            index_performance.HarnessError,
            "measured_iterations",
        ):
            self.validate(value)

        value = copy.deepcopy(self.manifest)
        value["cases"][0] = None
        with self.assertRaisesRegex(
            index_performance.HarnessError,
            r"cases\[0\]",
        ):
            self.validate(value)

    def test_common_harness_configuration_is_index_scoped(self) -> None:
        index_performance._configure_common()
        self.assertEqual(
            index_performance.CASE_IDS,
            read_performance.EXPECTED_CASE_IDS,
        )
        self.assertEqual(
            index_performance.MINIMUM_WALL_NS,
            read_performance.MINIMUM_WALL_NS,
        )
        self.assertEqual(260, read_performance.MAX_BASELINE_ARTIFACTS)
        self.assertTrue(read_performance.ENFORCE_GUARD_ON_VALIDATION)
        case = self.manifest["cases"][0]
        self.assertEqual(
            case["expected"]["smoke"],
            read_performance._smoke_work(self.manifest, case),
        )


if __name__ == "__main__":
    unittest.main()
