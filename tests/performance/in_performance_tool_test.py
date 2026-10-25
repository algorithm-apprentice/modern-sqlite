import copy
import pathlib
import unittest

from tools import in_performance
from tools import order_by_performance
from tools import read_performance


class InPerformanceValidationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = pathlib.Path(__file__).resolve().parents[2]
        cls.path = cls.root / "tests/performance/in-workloads-v1.json"
        cls.manifest = read_performance.load_json_strict(cls.path)

    def validate(self, value: object) -> dict[str, object]:
        return in_performance.validate_workload_manifest(
            value, repository_root=self.root, manifest_path=self.path
        )

    def diagnostic(self, engine: str) -> dict:
        case = self.manifest["cases"][0]
        profile = read_performance.load_json_strict(self.root / self.manifest["sqlite_profile"])
        names = (
            read_performance.MODERN_COUNTER_NAMES if engine == "modern"
            else order_by_performance.ORDER_SQLITE_COUNTER_NAMES
        )
        counters = dict.fromkeys(names, 1)
        for name in (
            "pages_written", "pages_read", "cache_misses", "cache_writes",
            "sort_operations", "temp_bytes_spilled", "fullscan_steps", "reprepares",
        ):
            if name in counters:
                counters[name] = 0
        if engine == "sqlite":
            counters["fullscan_steps"] = 65535
        return {
            "schema_version": 1, "completion_schema_version": 1,
            "diagnostic_schema_version": 1, "workload_semantics_version": 1,
            "mode": "diagnostic", "engine": engine, "case": case["id"],
            "build": {
                "build_type": "Release", "instrumentation": True,
                "sanitizers": False, "coverage": False,
            },
            "sqlite": {
                "version": profile["sqlite"]["version"],
                "source_id": profile["sqlite"]["source_id"],
                "compile_options": self.manifest["sqlite_semantic_compile_options"],
            },
            "effective_configuration":
                order_by_performance._expected_case_configuration(self.manifest, case),
            "completion": {
                "status": "complete", "session_opens": 1,
                "statement_prepares": 4, "statement_finalizes": 4, "statement_resets": 4,
                "pre_verifications": 1, "post_verifications": 1,
            },
            "counters": {
                "modern": counters if engine == "modern" else {},
                "sqlite": counters if engine == "sqlite" else {},
            },
            "work": copy.deepcopy(case["expected"]["diagnostic"]),
        }

    def test_pinned_manifest_and_independent_vectors_are_valid(self) -> None:
        validated = self.validate(copy.deepcopy(self.manifest))
        self.assertEqual(
            in_performance.CASE_IDS,
            tuple(case["id"] for case in validated["cases"]),
        )
        self.assertEqual(5, len(validated["cases"]))
        self.assertEqual(8, validated["cases"][0]["measured_iterations"])

    def test_rejects_sql_work_scale_budget_digest_and_guard_drift(self) -> None:
        mutations = (
            (("cases", 0, "sql"), "SELECT 1"),
            (("cases", 0, "measured_iterations"), 1),
            (("cases", 0, "expected", "measured", "rows"), 1),
            (("cases", 0, "expected", "smoke", "digest"), "0" * 16),
            (("cases", 2, "temporary_store"), "memory"),
            (("cases", 2, "sorter_memory_threshold"), 1),
            (("guard", "maximum_cpu_ratio", "numerator"), 11),
            (("minimum_wall_ns",), 1),
        )
        for path, replacement in mutations:
            with self.subTest(path=path):
                value = copy.deepcopy(self.manifest)
                target = value
                for key in path[:-1]:
                    target = target[key]
                target[path[-1]] = replacement
                with self.assertRaises(in_performance.HarnessError):
                    self.validate(value)

    def test_rejects_boolean_numeric_fields_and_fixture_drift(self) -> None:
        value = copy.deepcopy(self.manifest)
        value["cases"][0]["measured_iterations"] = True
        with self.assertRaises(in_performance.HarnessError):
            self.validate(value)
        value = copy.deepcopy(self.manifest)
        value["fixtures"][0]["sha256"] = "0" * 64
        with self.assertRaises(in_performance.HarnessError):
            self.validate(value)

    def test_contract_context_restores_existing_family_and_common_state(self) -> None:
        def snapshot() -> tuple:
            names = (
                "EXPECTED_CASE_IDS", "MINIMUM_WALL_NS", "MAX_BASELINE_ARTIFACTS",
                "ENFORCE_GUARD_ON_VALIDATION", "validate_workload_manifest", "_smoke_work",
                "_collect_input_references", "validate_raw_timing_report",
                "validate_raw_diagnostic_report", "_capture_actual_build_flags",
            )
            return (
                order_by_performance.CASE_IDS, order_by_performance._CASE_CONTRACTS,
                order_by_performance.MINIMUM_WALL_NS,
                order_by_performance.validate_workload_manifest,
                order_by_performance.validate_raw_timing_report,
                order_by_performance.validate_raw_diagnostic_report,
                order_by_performance._collect_input_references,
                tuple(getattr(read_performance, name) for name in names),
            )
        before = snapshot()
        with in_performance.configured_contract():
            self.assertEqual(in_performance.CASE_IDS, order_by_performance.CASE_IDS)
            self.assertEqual(in_performance.CASE_IDS, read_performance.EXPECTED_CASE_IDS)
            with in_performance.configured_contract():
                self.assertEqual(in_performance.CASE_IDS, order_by_performance.CASE_IDS)
        self.assertEqual(before, snapshot())
        with self.assertRaisesRegex(RuntimeError, "control"):
            with in_performance.configured_contract():
                raise RuntimeError("control")
        self.assertEqual(before, snapshot())

    def test_source_identity_includes_the_actual_family_adapter(self) -> None:
        references = in_performance.collect_input_references(
            repository_root=self.root,
            workload_path=self.path,
            workload_manifest=self.manifest,
        )
        selected = [item for item in references if item["role"] == "in-list-runner-source"]
        self.assertEqual(1, len(selected))
        self.assertEqual("tools/in_performance.py", selected[0]["path"])
        self.assertEqual(1, sum(item["role"] == "shared-contract-source" for item in references))

    def test_native_in_workloads_reject_sorting_instead_of_masking_it(self) -> None:
        report = self.diagnostic("sqlite")
        original = copy.deepcopy(report)
        in_performance.validate_raw_diagnostic_report(
            report, workload_manifest=self.manifest,
            expected_engine="sqlite", expected_case=in_performance.CASE_IDS[0],
        )
        self.assertEqual(original, report)
        report["counters"]["sqlite"]["sort_operations"] = 1
        with self.assertRaisesRegex(in_performance.HarnessError, "sort"):
            in_performance.validate_raw_diagnostic_report(
                report,
                workload_manifest=self.manifest,
                expected_engine="sqlite",
                expected_case=in_performance.CASE_IDS[0],
            )

    def test_rejects_actual_native_work_spill_and_typed_sort_counter_drift(self) -> None:
        for name, replacement in (
            ("fullscan_steps", 0), ("temp_bytes_spilled", 1),
            ("sort_operations", False), ("sort_operations", 0.0),
        ):
            with self.subTest(name=name, replacement=replacement):
                report = self.diagnostic("sqlite")
                report["counters"]["sqlite"][name] = replacement
                with self.assertRaises(in_performance.HarnessError):
                    in_performance.validate_raw_diagnostic_report(
                        report, workload_manifest=self.manifest,
                        expected_engine="sqlite", expected_case=in_performance.CASE_IDS[0],
                    )
        report = self.diagnostic("modern")
        in_performance.validate_raw_diagnostic_report(
            report, workload_manifest=self.manifest,
            expected_engine="modern", expected_case=in_performance.CASE_IDS[0],
        )
        report["counters"]["modern"]["pages_read"] = 1
        with self.assertRaises(in_performance.HarnessError):
            in_performance.validate_raw_diagnostic_report(
                report, workload_manifest=self.manifest,
                expected_engine="modern", expected_case=in_performance.CASE_IDS[0],
            )


if __name__ == "__main__":
    unittest.main()
