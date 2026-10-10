# SQLite 3.54.0 Read Compatibility Fixture

`sqlite-3.54.0-read-compatibility.db` is the pinned write-side fixture for
the read compatibility, deterministic model, and fuzz smoke tests. It was
generated with SQLite 3.54.0 at Fossil check-in
`65ec11f05a9ee5b23495427ece76c0550a8ffc28980a8a0c72d556ba0d2290e2`.

The database uses 512-byte pages, rollback-journal `DELETE` mode, and no
auto-vacuum. It covers rowid and WITHOUT ROWID tables, all five SQLite
storage classes, embedded NUL text, empty and overflow payloads, deterministic
model rows, and records created before `ALTER TABLE ADD COLUMN`.

## Canonical SQLite builds

Generate `sqlite3.c`, `sqlite3.h`, and `shell.c` from the pinned source, then
build the regeneration library with the semantic profile recorded in
`tests/compatibility/sqlite-oracle-profile-v1.json`.
The profile requires exact equality for the normalized semantic compile
options; compiler-identification entries are diagnostic-only and are not
stored in the oracle.

macOS shared library:

```sh
clang -O2 -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_DQS=3 \
  -fPIC -dynamiclib sqlite3.c \
  -o libsqlite3-read-compat-profile.dylib
```

Linux shared library:

```sh
cc -O2 -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_DQS=3 \
  -fPIC -shared sqlite3.c -lm -ldl -lpthread \
  -o libsqlite3-read-compat-profile.so
```

Build the matching shell used only to create the database:

```sh
cc -O2 -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_DQS=3 \
  shell.c sqlite3.c -lm -o sqlite3-read-compat-profile
```

## Fixture generation and integrity

From the repository root:

```sh
rm -f tests/fixtures/read_compatibility/sqlite-3.54.0-read-compatibility.db
./sqlite3-read-compat-profile \
  tests/fixtures/read_compatibility/sqlite-3.54.0-read-compatibility.db \
  < tests/fixtures/read_compatibility/read-compatibility.sql

printf 'PRAGMA integrity_check;\nPRAGMA page_size;\nPRAGMA journal_mode;\n' |
  ./sqlite3-read-compat-profile \
    tests/fixtures/read_compatibility/sqlite-3.54.0-read-compatibility.db
```

The expected diagnostic output is:

```text
ok
512
delete
```

## Oracle regeneration

The pinned shared library is explicit regeneration input. It is not committed
and is not required by ordinary compatibility verification.

```sh
python3 tools/read_compatibility.py regenerate \
  --repository-root . \
  --corpus tests/compatibility/read-corpus-v1.json \
  --output tests/compatibility/read-oracle-v1.json \
  --sqlite-library ./libsqlite3-read-compat-profile.dylib \
  --sqlite-c ./sqlite3.c \
  --sqlite-h ./sqlite3.h
```

To enable direct extraction and clean-regeneration CTests, configure with all
three pinned inputs:

```sh
cmake --preset dev-debug \
  -DMODERN_SQLITE_PINNED_SQLITE_LIBRARY="$PWD/libsqlite3-read-compat-profile.dylib" \
  -DMODERN_SQLITE_PINNED_SQLITE_C="$PWD/sqlite3.c" \
  -DMODERN_SQLITE_PINNED_SQLITE_H="$PWD/sqlite3.h"
```

## SHA-256

```text
4cee66a6b5eecaf9a016fd62bdc64be75a1f5ab1c34c5881a539fdca639fada6  sqlite3.c
ab12aaa090d3921ba8fd2c8c198ae1e8cafce79fd9897a5b89830fab1f6ecef5  sqlite3.h
6da29de262fa702a09fdd4030417c93405258f5cfbce652b341bbcf4ec798d1a  tests/fixtures/read_compatibility/read-compatibility.sql
9e02af35427a93406676902ba81333ab82b66d3583e82d0195b6ff0ae8474905  tests/fixtures/read_compatibility/sqlite-3.54.0-read-compatibility.db
cf0982cb2f38c893359188a6ed11bd05ff541d5975a57d482f7fd367a4d02981  tests/compatibility/sqlite-oracle-profile-v1.json
063abbc099e5ebcee276e184e1ebe989fe6ef40deba8825171461b93527e2cc4  tests/compatibility/read-corpus-v1.json
654b5a7f7ed1e6bbecd382a1b3bd7e49d9b7f266c4ee3412b3ecce36fd45c751  tests/compatibility/read-oracle-v1.json
24f4c5b458af493128219c1961a6d13cef7c99278c24527e21a2a0f546688592  tools/read_compatibility.py
```
