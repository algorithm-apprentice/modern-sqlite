import pathlib
import re
import unittest


class VmLayeringTest(unittest.TestCase):
    def test_public_header_exposes_only_direct_runtime_contracts(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (root / "include/modern_sqlite/vm/vm.hpp").read_text(
            encoding="utf-8"
        )
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(
            {
                "modern_sqlite/base/result.hpp",
                "modern_sqlite/bytecode/program.hpp",
            },
            includes,
        )

    def test_source_does_not_reach_into_compiler_or_session_layers(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        source = (root / "src/vm/vm.cpp").read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', source, flags=re.MULTILINE)
        )
        forbidden_prefixes = (
            "modern_sqlite/api/",
            "modern_sqlite/binder/",
            "modern_sqlite/catalog/",
            "modern_sqlite/diagnostics/",
            "modern_sqlite/lowering/",
            "modern_sqlite/planner/",
            "modern_sqlite/session/",
            "modern_sqlite/syntax/",
        )
        for include in includes:
            self.assertFalse(include.startswith(forbidden_prefixes), include)


if __name__ == "__main__":
    unittest.main()
