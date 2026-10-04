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


if __name__ == "__main__":
    unittest.main()
