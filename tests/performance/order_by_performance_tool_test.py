#!/usr/bin/env python3

import copy
import pathlib
import sys
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import order_by_performance
from tools import read_performance


class OrderByPerformanceValidationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = REPOSITORY_ROOT
        cls.manifest_path = (
            cls.root / "tests/performance/order-by-workloads-v1.json"
        )
        cls.manifest = read_performance.load_json_strict(cls.manifest_path)

    def validate(self, value: object) -> dict[str, object]:
        return order_by_performance.validate_workload_manifest(
            value,
            repository_root=self.root,
            manifest_path=self.manifest_path,
        )

    def test_canonical_manifest_is_valid(self) -> None:
        validated = self.validate(copy.deepcopy(self.manifest))
        self.assertEqual(
            order_by_performance.CASE_IDS,
            tuple(case["id"] for case in validated["cases"]),
        )
        self.assertEqual(
            order_by_performance.MINIMUM_WALL_NS,
            validated["minimum_wall_ns"],
        )

    def test_rejects_semantic_fixture_and_temporary_storage_drift(self) -> None:
        cases = (
            ("minimum_wall_ns", 1, "minimum wall"),
            ("cases", [], "case IDs"),
        )
        for key, replacement, message in cases:
            with self.subTest(key=key):
                value = copy.deepcopy(self.manifest)
                value[key] = replacement
                with self.assertRaisesRegex(
                    order_by_performance.HarnessError,
                    message,
                ):
                    self.validate(value)

        value = copy.deepcopy(self.manifest)
        value["fixtures"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(
            order_by_performance.HarnessError,
            "fixture metadata",
        ):
            self.validate(value)

        value = copy.deepcopy(self.manifest)
        value["cases"][4]["sorter_memory_threshold"] += 1
        with self.assertRaisesRegex(
            order_by_performance.HarnessError,
            "sorter_memory_threshold",
        ):
            self.validate(value)

        value = copy.deepcopy(self.manifest)
        value["cases"][8]["temporary_store"] = "memory"
        with self.assertRaisesRegex(
            order_by_performance.HarnessError,
            "temporary_store",
        ):
            self.validate(value)

    def test_common_harness_configuration_is_order_by_scoped(self) -> None:
        order_by_performance._configure_common()
        self.assertEqual(
            order_by_performance.CASE_IDS,
            read_performance.EXPECTED_CASE_IDS,
        )
        self.assertEqual(
            order_by_performance.MINIMUM_WALL_NS,
            read_performance.MINIMUM_WALL_NS,
        )
        self.assertEqual(200, read_performance.MAX_BASELINE_ARTIFACTS)
        self.assertTrue(read_performance.ENFORCE_GUARD_ON_VALIDATION)
        case = self.manifest["cases"][0]
        self.assertEqual(
            case["expected"]["smoke"],
            read_performance._smoke_work(self.manifest, case),
        )


if __name__ == "__main__":
    unittest.main()
