#!/usr/bin/env python3

import copy
import pathlib
import sys
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import distinct_compound_performance
from tools import read_performance


class DistinctCompoundPerformanceValidationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = REPOSITORY_ROOT
        cls.manifest_path = (
            cls.root / "tests/performance/distinct-compound-workloads-v1.json"
        )
        cls.manifest = read_performance.load_json_strict(cls.manifest_path)

    @classmethod
    def tearDownClass(cls) -> None:
        distinct_compound_performance._restore_common()

    def validate(self, value: object) -> dict[str, object]:
        return distinct_compound_performance.validate_workload_manifest(
            value,
            repository_root=self.root,
            manifest_path=self.manifest_path,
        )

    def test_canonical_manifest_is_valid(self) -> None:
        validated = self.validate(copy.deepcopy(self.manifest))
        self.assertEqual(
            distinct_compound_performance.CASE_IDS,
            tuple(case["id"] for case in validated["cases"]),
        )
        self.assertEqual(
            distinct_compound_performance.MINIMUM_WALL_NS,
            validated["minimum_wall_ns"],
        )

    def test_rejects_case_fixture_and_storage_drift(self) -> None:
        for key, replacement, message in (
            ("minimum_wall_ns", 1, "minimum wall"),
            ("cases", [], "case IDs"),
        ):
            with self.subTest(key=key):
                value = copy.deepcopy(self.manifest)
                value[key] = replacement
                with self.assertRaisesRegex(
                    distinct_compound_performance.HarnessError,
                    message,
                ):
                    self.validate(value)

        value = copy.deepcopy(self.manifest)
        value["fixtures"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(
            distinct_compound_performance.HarnessError,
            "fixture metadata",
        ):
            self.validate(value)

        value = copy.deepcopy(self.manifest)
        value["cases"][5]["sorter_memory_threshold"] += 1
        with self.assertRaisesRegex(
            distinct_compound_performance.HarnessError,
            "sorter_memory_threshold",
        ):
            self.validate(value)

        value = copy.deepcopy(self.manifest)
        value["cases"][-1]["temporary_store"] = "memory"
        with self.assertRaisesRegex(
            distinct_compound_performance.HarnessError,
            "temporary_store",
        ):
            self.validate(value)

    def test_common_harness_configuration_is_contract_scoped(self) -> None:
        distinct_compound_performance._configure_common()
        self.assertEqual(
            distinct_compound_performance.CASE_IDS,
            read_performance.EXPECTED_CASE_IDS,
        )
        self.assertEqual(
            distinct_compound_performance.MINIMUM_WALL_NS,
            read_performance.MINIMUM_WALL_NS,
        )
        self.assertEqual(300, read_performance.MAX_BASELINE_ARTIFACTS)
        self.assertTrue(read_performance.ENFORCE_GUARD_ON_VALIDATION)
        case = self.manifest["cases"][0]
        self.assertEqual(
            case["expected"]["smoke"],
            read_performance._smoke_work(self.manifest, case),
        )

    def test_rejects_measured_work_schema_and_digest_drift(self) -> None:
        missing = copy.deepcopy(self.manifest)
        del missing["cases"][0]["expected"]["measured"]["rows"]
        with self.assertRaisesRegex(
            distinct_compound_performance.HarnessError,
            "expected.measured",
        ):
            self.validate(missing)

        changed = copy.deepcopy(self.manifest)
        changed["cases"][0]["expected"]["measured"]["digest"] = "0" * 16
        with self.assertRaisesRegex(
            distinct_compound_performance.HarnessError,
            "pinned work",
        ):
            self.validate(changed)


if __name__ == "__main__":
    unittest.main()
