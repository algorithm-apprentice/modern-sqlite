import pathlib
import re
import unittest


class CatalogLayeringTest(unittest.TestCase):
    def test_catalog_header_depends_only_on_direct_completed_prerequisites(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (root / "include/modern_sqlite/catalog/catalog.hpp").read_text(
            encoding="utf-8"
        )
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(
            {
                "modern_sqlite/runtime/sql_value.hpp",
                "modern_sqlite/syntax/ast.hpp",
            },
            includes,
        )

    def test_catalog_loader_header_exposes_only_direct_contracts(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (
            root / "include/modern_sqlite/catalog/catalog_loader.hpp"
        ).read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(
            {
                "modern_sqlite/base/result.hpp",
                "modern_sqlite/catalog/catalog.hpp",
                "modern_sqlite/pager/pager.hpp",
            },
            includes,
        )

    def test_catalog_loader_source_does_not_reach_into_upper_layers(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        source = (root / "src/catalog/catalog_loader.cpp").read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', source, flags=re.MULTILINE)
        )
        forbidden_prefixes = (
            "modern_sqlite/api/",
            "modern_sqlite/binder/",
            "modern_sqlite/diagnostics/",
            "modern_sqlite/planner/",
            "modern_sqlite/session/",
            "modern_sqlite/vm/",
        )
        for include in includes:
            self.assertFalse(include.startswith(forbidden_prefixes), include)


if __name__ == "__main__":
    unittest.main()
