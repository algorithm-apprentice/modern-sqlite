import pathlib
import re
import unittest


class SyntaxLayeringTest(unittest.TestCase):
    def test_ast_header_depends_only_on_completed_lower_layers(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (root / "include/modern_sqlite/syntax/ast.hpp").read_text(
            encoding="utf-8"
        )
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(
            {
                "modern_sqlite/base/result.hpp",
                "modern_sqlite/text/text.hpp",
            },
            includes,
        )


if __name__ == "__main__":
    unittest.main()
