import copy
import pathlib
import sys
import unittest

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import aggregate_performance
from tools import read_performance


class AggregatePerformanceValidationTest(unittest.TestCase):
    def manifest(self):
        path = REPOSITORY_ROOT / "tests/performance/aggregate-workloads-v1.json"
        return path, read_performance.load_json_strict(path)

    def validate(self, value, path):
        return aggregate_performance.validate_workload_manifest(
            value, repository_root=REPOSITORY_ROOT, manifest_path=path
        )

    def diagnostic(self, case_id="indexed-min-memory", engine="sqlite"):
        _, manifest = self.manifest()
        case = next(case for case in manifest["cases"] if case["id"] == case_id)
        profile = read_performance.load_json_strict(
            REPOSITORY_ROOT / manifest["sqlite_profile"]
        )
        counter_names = (
            read_performance.MODERN_COUNTER_NAMES if engine == "modern"
            else aggregate_performance.base.ORDER_SQLITE_COUNTER_NAMES
        )
        counters = dict.fromkeys(counter_names, 1)
        for name in ("pages_written", "pages_read", "cache_misses", "cache_writes",
                     "sort_operations", "temp_bytes_spilled", "fullscan_steps", "reprepares"):
            if name in counters:
                counters[name] = 0
        if engine == "sqlite":
            counters["fullscan_steps"] = aggregate_performance.MECHANISMS[case_id][0]
            counters["sort_operations"] = aggregate_performance.MECHANISMS[case_id][1]
        work = {}
        if engine == "modern":
            work = aggregate_performance.expected_operations(case_id)
            work.update(dict.fromkeys(aggregate_performance.PEAK_NAMES, 0))
        report = {
            "schema_version": 1, "completion_schema_version": 1,
            "diagnostic_schema_version": 1, "workload_semantics_version": 1,
            "mode": "diagnostic", "engine": engine, "case": case_id,
            "build": {"build_type": "Release", "coverage": False,
                      "instrumentation": True, "sanitizers": False},
            "sqlite": {"version": profile["sqlite"]["version"],
                       "source_id": profile["sqlite"]["source_id"],
                       "compile_options": manifest["sqlite_semantic_compile_options"]},
            "effective_configuration":
                aggregate_performance.base._expected_case_configuration(manifest, case),
            "completion": {
                "session_opens": 1, "statement_prepares": 4, "statement_finalizes": 4,
                "statement_resets": 4, "pre_verifications": 1,
                "post_verifications": 1, "status": "complete",
            },
            "counters": {"modern": counters if engine == "modern" else {},
                         "sqlite": counters if engine == "sqlite" else {}},
            "work": copy.deepcopy(case["expected"]["diagnostic"]),
            "probe_counts": aggregate_performance.expected_probe_counts(case_id),
            "aggregate_work": work,
        }
        return manifest, report

    def test_rejects_indexed_access_and_ordering_mechanism_drift(self) -> None:
        manifest, report = self.diagnostic()
        aggregate_performance.validate_raw_diagnostic_report(
            copy.deepcopy(report), workload_manifest=manifest,
            expected_engine="sqlite", expected_case="indexed-min-memory",
        )
        for name, value in (("fullscan_steps", 65535), ("sort_operations", 1), ("vm_steps", 1_000_000)):
            with self.subTest(name=name):
                changed = copy.deepcopy(report)
                changed["counters"]["sqlite"][name] = value
                with self.assertRaises(aggregate_performance.HarnessError):
                    aggregate_performance.validate_raw_diagnostic_report(
                        changed, workload_manifest=manifest,
                        expected_engine="sqlite", expected_case="indexed-min-memory",
                    )

    def test_missing_timing_configuration_is_a_harness_error(self) -> None:
        _, manifest = self.manifest()
        malformed = {
            "schema_version": 1, "completion_schema_version": 1,
            "workload_semantics_version": 1,
        }
        with self.assertRaises(aggregate_performance.HarnessError):
            aggregate_performance.validate_raw_timing_report(
                malformed, workload_manifest=manifest,
                expected_engine="modern", expected_case="global-functions-memory",
            )

    def test_rejects_ordered_and_sorted_storage_mechanism_drift(self) -> None:
        manifest, ordered = self.diagnostic("ordered-group-memory", "modern")
        aggregate_performance.validate_raw_diagnostic_report(
            copy.deepcopy(ordered), workload_manifest=manifest,
            expected_engine="modern", expected_case="ordered-group-memory",
        )
        ordered["aggregate_work"]["retained_bytes_peak"] = 1024
        with self.assertRaises(aggregate_performance.HarnessError):
            aggregate_performance.validate_raw_diagnostic_report(
                ordered, workload_manifest=manifest,
                expected_engine="modern", expected_case="ordered-group-memory",
            )
        manifest, sorted_report = self.diagnostic("sorted-group-memory", "sqlite")
        sorted_report["counters"]["sqlite"]["sort_operations"] = 0
        with self.assertRaises(aggregate_performance.HarnessError):
            aggregate_performance.validate_raw_diagnostic_report(
                sorted_report, workload_manifest=manifest,
                expected_engine="sqlite", expected_case="sorted-group-memory",
            )

    def test_rejects_native_temp_buffer_classification_drift(self) -> None:
        manifest, report = self.diagnostic("sorted-group-file", "sqlite")
        report["counters"]["sqlite"]["temp_bytes_spilled"] = 4096
        with self.assertRaises(aggregate_performance.HarnessError):
            aggregate_performance.validate_raw_diagnostic_report(
                report, workload_manifest=manifest,
                expected_engine="sqlite", expected_case="sorted-group-file",
            )

    def test_rejects_file_writes_without_a_live_file_peak(self) -> None:
        manifest, report = self.diagnostic("sorted-group-file", "modern")
        report["aggregate_work"].update(
            retained_bytes_peak=1, file_acquisitions=1,
            sorter_spill_writes=1, live_files_peak=0,
        )
        with self.assertRaises(aggregate_performance.HarnessError):
            aggregate_performance.validate_raw_diagnostic_report(
                report, workload_manifest=manifest,
                expected_engine="modern", expected_case="sorted-group-file",
            )

    def test_canonical_manifest_is_valid(self) -> None:
        path = REPOSITORY_ROOT / "tests/performance/aggregate-workloads-v1.json"
        manifest = read_performance.load_json_strict(path)
        validated = aggregate_performance.validate_workload_manifest(
            copy.deepcopy(manifest), repository_root=REPOSITORY_ROOT, manifest_path=path
        )
        self.assertEqual(
            aggregate_performance.CASE_IDS, tuple(case["id"] for case in validated["cases"])
        )
        self.assertEqual(40, validated["guard"]["maximum_cpu_ratio"]["numerator"])
        self.assertEqual(5_000_000, validated["minimum_wall_ns"])

    def test_rejects_work_and_state_drift(self) -> None:
        path, manifest = self.manifest()
        for phase, field, replacement in (
            ("warmup", "rows", 2), ("measured", "digest", "0" * 16),
            ("diagnostic", "items", 1), ("smoke", "bytes", 49),
            ("verification", "digest", "0" * 16),
        ):
            with self.subTest(phase=phase):
                value = copy.deepcopy(manifest)
                value["cases"][0]["expected"][phase][field] = replacement
                with self.assertRaises(aggregate_performance.HarnessError):
                    self.validate(value, path)
        value = copy.deepcopy(manifest)
        value["cases"][4]["aggregate_operations"]["reset"] -= 1
        with self.assertRaises(aggregate_performance.HarnessError):
            self.validate(value, path)
        value = copy.deepcopy(manifest)
        value["cases"][0]["diagnostic_probe_groups"][0]["calls_per_tag"] -= 1
        with self.assertRaises(aggregate_performance.HarnessError):
            self.validate(value, path)

    def test_rejects_types_and_configuration_drift(self) -> None:
        path, manifest = self.manifest()
        for name, mutate in (
            ("schema", lambda value: value.__setitem__("schema_version", 1.0)),
            ("page", lambda value: value["configuration"].__setitem__("page_size", 4096.0)),
            ("guard", lambda value: value["guard"]["maximum_cpu_ratio"].__setitem__("denominator", True)),
            ("iterations", lambda value: value["cases"][0].__setitem__("measured_iterations", True)),
            ("budget", lambda value: value["cases"][6].__setitem__("sorter_memory_threshold", 1)),
        ):
            with self.subTest(name=name):
                value = copy.deepcopy(manifest)
                mutate(value)
                with self.assertRaises(aggregate_performance.HarnessError):
                    self.validate(value, path)

    def test_empty_work_and_fast_lookup_repetition_are_pinned(self) -> None:
        path, manifest = self.manifest()
        self.validate(copy.deepcopy(manifest), path)
        self.assertEqual(0, manifest["cases"][1]["items_per_iteration"])
        for case in manifest["cases"][1:4]:
            self.assertEqual(65536, case["measured_iterations"])
            self.assertEqual(1, case["diagnostic_iterations"])

    def test_rejects_missing_baseline_without_fallback(self) -> None:
        path, _ = self.manifest()
        aggregate_performance._configure_common()
        try:
            with self.assertRaisesRegex(read_performance.HarnessError, "not a directory"):
                read_performance.validate_baseline_directory(
                    baseline_path=REPOSITORY_ROOT / "benchmarks/aggregate-baseline-does-not-exist",
                    repository_root=REPOSITORY_ROOT, workload_path=path,
                )
        finally:
            aggregate_performance._restore_common()


if __name__ == "__main__":
    unittest.main()
