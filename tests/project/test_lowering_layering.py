import pathlib
import re
import unittest


class LoweringLayeringTest(unittest.TestCase):
    def test_public_header_exposes_only_direct_lowering_contracts(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (
            root / "include/modern_sqlite/lowering/read_lowering.hpp"
        ).read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(
            {
                "modern_sqlite/base/result.hpp",
                "modern_sqlite/bytecode/program.hpp",
                "modern_sqlite/optimizer/physical_plan.hpp",
            },
            includes,
        )

    def test_source_does_not_reach_into_execution_or_session_layers(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        source = (root / "src/lowering/read_lowering.cpp").read_text(
            encoding="utf-8"
        )
        includes = set(
            re.findall(r'^#include "([^"]+)"', source, flags=re.MULTILINE)
        )
        forbidden_prefixes = (
            "modern_sqlite/api/",
            "modern_sqlite/diagnostics/",
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
