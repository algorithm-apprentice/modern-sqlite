import pathlib
import re
import unittest


class OptimizerLayeringTest(unittest.TestCase):
    def test_public_header_exposes_only_direct_optimizer_contracts(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (
            root / "include/modern_sqlite/optimizer/physical_plan.hpp"
        ).read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(
            {
                "modern_sqlite/base/result.hpp",
                "modern_sqlite/planner/logical_plan.hpp",
            },
            includes,
        )

    def test_source_does_not_reach_into_lowering_or_execution_layers(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        source = (root / "src/optimizer/physical_plan.cpp").read_text(
            encoding="utf-8"
        )
        includes = set(
            re.findall(r'^#include "([^"]+)"', source, flags=re.MULTILINE)
        )
        forbidden_prefixes = (
            "modern_sqlite/api/",
            "modern_sqlite/bytecode/",
            "modern_sqlite/diagnostics/",
            "modern_sqlite/format/",
            "modern_sqlite/lowering/",
            "modern_sqlite/pager/",
            "modern_sqlite/platform/",
            "modern_sqlite/session/",
            "modern_sqlite/storage/",
            "modern_sqlite/vm/",
        )
        for include in includes:
            self.assertFalse(include.startswith(forbidden_prefixes), include)


if __name__ == "__main__":
    unittest.main()
