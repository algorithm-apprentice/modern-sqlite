PRAGMA page_size=512;
PRAGMA auto_vacuum=NONE;
PRAGMA journal_mode=DELETE;
VACUUM;

CREATE TABLE items(
  id INTEGER PRIMARY KEY,
  name TEXT COLLATE NOCASE NOT NULL,
  score REAL,
  category TEXT,
  payload BLOB,
  note TEXT,
  quantity INTEGER
);

INSERT INTO items VALUES
  (1, 'alpha', 1.5, 'one', X'0001ff', 'plain', -7),
  (2, 'beta', 3.0, 'two', X'', CAST(X'610062' AS TEXT), 0),
  (3, 'gamma', NULL, NULL, NULL, '', 9223372036854775807),
  (4, 'Delta', -0.0, 'four', X'ff00', 'trailing   ', -9223372036854775808);

CREATE TABLE storage_values(
  id INTEGER PRIMARY KEY,
  value
);

INSERT INTO storage_values VALUES
  (1, NULL),
  (2, -9223372036854775808),
  (3, 9223372036854775807),
  (4, 1.25),
  (5, 'text'),
  (6, X'00ff10'),
  (7, CAST(X'610062' AS TEXT)),
  (8, X''),
  (9, ''),
  (10, CAST(replace(hex(zeroblob(1200)), '00', 'x') AS TEXT)),
  (11, zeroblob(2000));

CREATE TABLE model_rows(
  id INTEGER PRIMARY KEY,
  value INTEGER NOT NULL,
  label TEXT NOT NULL,
  nullable INTEGER
);

INSERT INTO model_rows VALUES
  (1, -31, 'row-01', NULL),
  (2, -29, 'row-02', 20),
  (3, -23, 'row-03', 30),
  (4, -19, 'row-04', NULL),
  (5, -17, 'row-05', 50),
  (6, -13, 'row-06', 60),
  (7, -11, 'row-07', NULL),
  (8, -7, 'row-08', 80),
  (9, -5, 'row-09', 90),
  (10, -3, 'row-10', NULL),
  (11, -2, 'row-11', 110),
  (12, -1, 'row-12', 120),
  (13, 0, 'row-13', NULL),
  (14, 1, 'row-14', 140),
  (15, 2, 'row-15', 150),
  (16, 3, 'row-16', NULL),
  (17, 5, 'row-17', 170),
  (18, 7, 'row-18', 180),
  (19, 11, 'row-19', NULL),
  (20, 13, 'row-20', 200),
  (21, 17, 'row-21', 210),
  (22, 19, 'row-22', NULL),
  (23, 23, 'row-23', 230),
  (24, 29, 'row-24', 240),
  (25, 31, 'row-25', NULL),
  (26, 37, 'row-26', 260),
  (27, 41, 'row-27', 270),
  (28, 43, 'row-28', NULL),
  (29, 47, 'row-29', 290),
  (30, 53, 'row-30', 300),
  (31, 59, 'row-31', NULL),
  (32, 61, 'row-32', 320);

CREATE TABLE wr(
  a TEXT COLLATE NOCASE,
  b INTEGER,
  c TEXT,
  payload BLOB,
  PRIMARY KEY(a DESC, b)
) WITHOUT ROWID;

INSERT INTO wr VALUES
  ('left', 1, 'first', X'01'),
  ('right', 2, 'second', X'02'),
  ('middle', 3, 'third', X'03');

CREATE TABLE altered(
  id INTEGER PRIMARY KEY,
  existing TEXT
);

INSERT INTO altered VALUES
  (1, 'before-one'),
  (2, 'before-two');

ALTER TABLE altered ADD COLUMN text_default TEXT DEFAULT 'fallback';
ALTER TABLE altered ADD COLUMN real_default REAL DEFAULT 1.25;
ALTER TABLE altered ADD COLUMN null_default DEFAULT NULL;
ALTER TABLE altered ADD COLUMN integer_default INTEGER DEFAULT 7;

INSERT INTO altered VALUES
  (3, 'after', 'stored', 2.5, 'present', 9),
  (4, 'stored-null', NULL, NULL, NULL, NULL);

PRAGMA integrity_check;
