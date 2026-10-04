PRAGMA page_size=512;
PRAGMA auto_vacuum=NONE;
VACUUM;

CREATE TABLE items(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT COLLATE NOCASE NOT NULL ON CONFLICT FAIL DEFAULT 'unknown',
  score REAL CHECK(score >= 0),
  category TEXT,
  UNIQUE(name),
  UNIQUE(category DESC)
) STRICT;
CREATE INDEX items_score_idx
ON items(score DESC, lower(name) COLLATE RTRIM)
WHERE score > 0;

CREATE TABLE wr(
  a TEXT COLLATE NOCASE,
  b INTEGER,
  c TEXT,
  payload BLOB,
  PRIMARY KEY(a DESC, b),
  UNIQUE(c)
) WITHOUT ROWID, STRICT;
CREATE INDEX wr_c_idx ON wr(c DESC);

CREATE TABLE descending_pk(
  id INTEGER PRIMARY KEY DESC,
  payload BLOB
);

CREATE TABLE folded(
  value TEXT,
  UNIQUE(value ASC),
  UNIQUE(value DESC)
);

CREATE TABLE legacy_type(
  id INTEGER GENERATED ALWAYS PRIMARY KEY,
  payload TEXT
);

CREATE TABLE legacy_quoted(
  a TEXT,
  b INTEGER,
  UNIQUE('a')
);
CREATE INDEX legacy_quoted_b ON legacy_quoted('b');

INSERT INTO items(name, score, category) VALUES
  ('alpha', 1.5, 'one'),
  ('beta', 3.0, 'two'),
  ('gamma', NULL, NULL);
INSERT INTO wr VALUES
  ('left', 1, 'first', X'01'),
  ('right', 2, 'second', X'02');
INSERT INTO descending_pk VALUES(10, X'10'), (20, X'20');
INSERT INTO folded VALUES('folded');
INSERT INTO legacy_type(payload) VALUES('rowid alias');
INSERT INTO legacy_quoted VALUES('quoted', 7);

ANALYZE;
UPDATE sqlite_stat1
SET stat = '120 12 unordered noskipscan sz=24'
WHERE idx = 'items_score_idx';
