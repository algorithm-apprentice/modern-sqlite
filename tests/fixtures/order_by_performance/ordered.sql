PRAGMA page_size = 4096;
PRAGMA auto_vacuum = NONE;
PRAGMA journal_mode = DELETE;
PRAGMA encoding = 'UTF-8';
PRAGMA application_id = 1297305426;
PRAGMA user_version = 1;

BEGIN IMMEDIATE;
CREATE TABLE items(
  id INTEGER PRIMARY KEY,
  category TEXT NOT NULL,
  score INTEGER NOT NULL,
  flag INTEGER NOT NULL,
  payload BLOB NOT NULL
);
WITH RECURSIVE sequence(n) AS (
  VALUES(1)
  UNION ALL
  SELECT n + 1 FROM sequence WHERE n < 65536
)
INSERT INTO items(id, category, score, flag, payload)
SELECT
  n,
  printf('category-%08x', (n * 40503) % 65537),
  (n * 73) % 4096,
  n % 2,
  CAST(printf('%08x', n) || lower(hex(zeroblob(124))) AS BLOB)
FROM sequence;
CREATE INDEX items_score_desc ON items(score DESC);
COMMIT;

VACUUM;
ANALYZE;
