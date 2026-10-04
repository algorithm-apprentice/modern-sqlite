import pathlib
import re
import unittest


class BytecodeLayeringTest(unittest.TestCase):
    def test_public_header_depends_only_on_direct_contracts(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (
            root / "include/modern_sqlite/bytecode/program.hpp"
        ).read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(
            {
                "modern_sqlite/base/result.hpp",
                "modern_sqlite/runtime/sql_value.hpp",
            },
            includes,
        )

    def test_source_does_not_reach_into_upper_or_storage_layers(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        source = (root / "src/bytecode/program.cpp").read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', source, flags=re.MULTILINE)
        )
        forbidden_prefixes = (
            "modern_sqlite/api/",
            "modern_sqlite/binder/",
            "modern_sqlite/catalog/",
            "modern_sqlite/diagnostics/",
            "modern_sqlite/format/",
            "modern_sqlite/pager/",
            "modern_sqlite/planner/",
            "modern_sqlite/platform/",
            "modern_sqlite/session/",
            "modern_sqlite/storage/",
            "modern_sqlite/syntax/",
            "modern_sqlite/vm/",
        )
        for include in includes:
            self.assertFalse(include.startswith(forbidden_prefixes), include)


if __name__ == "__main__":
    unittest.main()
