import pathlib
import re
import unittest


class BinderLayeringTest(unittest.TestCase):
    def test_public_header_exposes_only_direct_binding_contracts(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (
            root / "include/modern_sqlite/binder/bound_select.hpp"
        ).read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(
            {
                "modern_sqlite/base/compiler_continuation.hpp",
                "modern_sqlite/base/result.hpp",
                "modern_sqlite/catalog/catalog.hpp",
                "modern_sqlite/runtime/sql_value.hpp",
                "modern_sqlite/syntax/ast.hpp",
                "modern_sqlite/text/text.hpp",
            },
            includes,
        )

    def test_internal_continuation_owner_has_no_higher_layer_dependencies(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (
            root / "include/modern_sqlite/base/compiler_continuation.hpp"
        ).read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(set(), includes)
        self.assertIn("namespace modern_sqlite::compiler_detail", header)
        self.assertFalse((root / "src/base/compiler_continuation.hpp").exists())

    def test_source_does_not_reach_into_planning_or_execution_layers(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        source = (root / "src/binder/bound_select.cpp").read_text(
            encoding="utf-8"
        )
        includes = set(
            re.findall(r'^#include "([^"]+)"', source, flags=re.MULTILINE)
        )
        forbidden_prefixes = (
            "modern_sqlite/api/",
            "modern_sqlite/bytecode/",
            "modern_sqlite/diagnostics/",
            "modern_sqlite/lowering/",
            "modern_sqlite/optimizer/",
            "modern_sqlite/pager/",
            "modern_sqlite/planner/",
            "modern_sqlite/platform/",
            "modern_sqlite/session/",
            "modern_sqlite/storage/",
            "modern_sqlite/vm/",
        )
        for include in includes:
            self.assertFalse(include.startswith(forbidden_prefixes), include)


if __name__ == "__main__":
    unittest.main()
