PRAGMA page_size = 4096;
PRAGMA auto_vacuum = NONE;
PRAGMA journal_mode = DELETE;
PRAGMA encoding = 'UTF-8';
PRAGMA application_id = 1297305936;
PRAGMA user_version = 1;

BEGIN IMMEDIATE;
CREATE TABLE kv(
  k INTEGER PRIMARY KEY,
  v BLOB NOT NULL
);
WITH RECURSIVE sequence(n) AS (
  VALUES(1)
  UNION ALL
  SELECT n + 1 FROM sequence WHERE n < 4096
)
INSERT INTO kv(k, v)
SELECT
  n * 2,
  CAST(printf('%08x', n) || lower(hex(zeroblob(124))) AS BLOB)
FROM sequence;
COMMIT;
VACUUM;
