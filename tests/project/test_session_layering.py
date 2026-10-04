import pathlib
import re
import unittest


class SessionLayeringTest(unittest.TestCase):
    def test_public_header_exposes_only_direct_session_contracts(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        header = (
            root / "include/modern_sqlite/session/read_session.hpp"
        ).read_text(encoding="utf-8")
        includes = set(
            re.findall(r'^#include "([^"]+)"', header, flags=re.MULTILINE)
        )
        self.assertSetEqual(
            {
                "modern_sqlite/base/bytes.hpp",
                "modern_sqlite/base/result.hpp",
                "modern_sqlite/bytecode/program.hpp",
                "modern_sqlite/platform/vfs.hpp",
                "modern_sqlite/runtime/sql_value.hpp",
                "modern_sqlite/text/text.hpp",
            },
            includes,
        )

    def test_lower_layers_do_not_depend_on_session(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        roots = (root / "include/modern_sqlite", root / "src")
        for source_root in roots:
            for path in source_root.rglob("*"):
                if path.suffix not in {".hpp", ".cpp"}:
                    continue
                if "session" in path.parts:
                    continue
                text = path.read_text(encoding="utf-8")
                self.assertNotIn("modern_sqlite/session/", text, str(path))

    def test_session_source_does_not_reach_into_future_layers(self) -> None:
        root = pathlib.Path(__file__).resolve().parents[2]
        source = root / "src/session/read_session.cpp"
        if not source.exists():
            return
        includes = set(
            re.findall(
                r'^#include "([^"]+)"',
                source.read_text(encoding="utf-8"),
                flags=re.MULTILINE,
            )
        )
        forbidden_prefixes = (
            "modern_sqlite/api/",
            "modern_sqlite/compatibility/",
            "modern_sqlite/diagnostics/",
            "modern_sqlite/performance/",
            "modern_sqlite/verification/",
        )
        for include in includes:
            self.assertFalse(include.startswith(forbidden_prefixes), include)


if __name__ == "__main__":
    unittest.main()
