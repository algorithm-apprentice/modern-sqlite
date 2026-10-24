from __future__ import annotations

import pathlib
import tempfile
import unittest

from tools import prepare_native_parser as adapter


class NativeParserAdapterTest(unittest.TestCase):
    def test_rewrites_only_the_two_root_semantic_actions(self) -> None:
        original = (
            "untouched lexer and Lemon tables\n"
            + adapter.SELECT_ACTION
            + "\nuntouched AST builders and destructors\n"
            + adapter.FINISH_ACTION
            + "\nuntouched parser teardown\n"
        )
        expected = original.replace(
            adapter.SELECT_ACTION, adapter.SYNTAX_SELECT_ACTION
        ).replace(adapter.FINISH_ACTION, adapter.SYNTAX_FINISH_ACTION)
        self.assertEqual(adapter.transform_source(original), expected)
        self.assertIn("sqlite3SelectDelete", adapter.SYNTAX_SELECT_ACTION)

    def test_missing_or_duplicate_anchor_is_an_error(self) -> None:
        for select_count, finish_count in ((0, 1), (2, 1), (1, 0), (1, 2)):
            source = (
                adapter.SELECT_ACTION * select_count
                + adapter.FINISH_ACTION * finish_count
            )
            with self.subTest(select=select_count, finish=finish_count):
                with self.assertRaisesRegex(adapter.AdapterError, "exactly once"):
                    adapter.transform_source(source)

    def test_wrong_reference_hash_does_not_create_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "sqlite3.c"
            output = root / "generated.c"
            source.write_text(adapter.SELECT_ACTION + adapter.FINISH_ACTION)
            with self.assertRaisesRegex(adapter.AdapterError, "SHA-256"):
                adapter.generate(source, output)
            self.assertFalse(output.exists())

    def test_reference_cannot_be_the_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "sqlite3.c"
            source.write_text("reference must remain unchanged")
            with self.assertRaisesRegex(adapter.AdapterError, "different"):
                adapter.generate(source, source)
            self.assertEqual(source.read_text(), "reference must remain unchanged")


if __name__ == "__main__":
    unittest.main()
