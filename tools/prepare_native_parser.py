#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys


SQLITE_SOURCE_SHA256 = (
    "4cee66a6b5eecaf9a016fd62bdc64be75a1f5ab1c34c5881a539fdca639fada6"
)
SYNTAX_SOURCE_SHA256 = (
    "aca609ee62d1dc2bb462ee19dc5b5ed9ef7dc9e832c3a45a1e1cee9f061f4d1b"
)
SELECT_ACTION = """  if( (pParse->db->mDbFlags & DBFLAG_EncodingFixed)!=0
   || sqlite3ReadSchema(pParse)==SQLITE_OK
  ){
    sqlite3Select(pParse, yymsp[0].minor.yy555, &dest);
  }
  sqlite3SelectDelete(pParse->db, yymsp[0].minor.yy555);"""
SYNTAX_SELECT_ACTION = """  if( !modern_syntax_only &&
      ((pParse->db->mDbFlags & DBFLAG_EncodingFixed)!=0
       || sqlite3ReadSchema(pParse)==SQLITE_OK)
  ){
    sqlite3Select(pParse, yymsp[0].minor.yy555, &dest);
  }
  sqlite3SelectDelete(pParse->db, yymsp[0].minor.yy555);"""
FINISH_ACTION = "{ sqlite3FinishCoding(pParse); }"
SYNTAX_FINISH_ACTION = """{ if( modern_syntax_only ){
    pParse->rc = SQLITE_DONE;
  }else{
    sqlite3FinishCoding(pParse);
  } }"""


class AdapterError(ValueError):
    pass


def transform_source(source: str) -> str:
    for anchor in (SELECT_ACTION, FINISH_ACTION):
        if source.count(anchor) != 1:
            raise AdapterError("each native parser action must occur exactly once")
    return source.replace(SELECT_ACTION, SYNTAX_SELECT_ACTION).replace(
        FINISH_ACTION, SYNTAX_FINISH_ACTION
    )


def generate(reference: pathlib.Path, output: pathlib.Path) -> None:
    if reference.resolve() == output.resolve():
        raise AdapterError("native parser output must be different from the reference")
    original = reference.read_bytes()
    if hashlib.sha256(original).hexdigest() != SQLITE_SOURCE_SHA256:
        raise AdapterError("native parser reference has the wrong SHA-256")
    modified = transform_source(original.decode("utf-8")).encode("utf-8")
    if hashlib.sha256(modified).hexdigest() != SYNTAX_SOURCE_SHA256:
        raise AdapterError("generated syntax adapter has the wrong SHA-256")
    if not output.exists() or output.read_bytes() != modified:
        output.write_bytes(modified)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the pinned SELECT syntax adapter")
    parser.add_argument("--reference", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    try:
        generate(arguments.reference, arguments.output)
    except (AdapterError, OSError, UnicodeError) as error:
        print(f"native parser adapter: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
