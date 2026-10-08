PRAGMA page_size=4096;
PRAGMA journal_mode=DELETE;
PRAGMA synchronous=FULL;
PRAGMA auto_vacuum=NONE;
PRAGMA encoding='UTF-8';
PRAGMA application_id=0;
PRAGMA user_version=1;

CREATE TABLE kv(
  k INTEGER PRIMARY KEY,
  v BLOB NOT NULL,
  version INTEGER NOT NULL
);

BEGIN;
WITH digits(d) AS (
  VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)
),
numbers(n) AS (
  SELECT 1 + d0.d + 10*d1.d + 100*d2.d + 1000*d3.d + 10000*d4.d
  FROM digits AS d0
  CROSS JOIN digits AS d1
  CROSS JOIN digits AS d2
  CROSS JOIN digits AS d3
  CROSS JOIN digits AS d4
)
INSERT INTO kv(k,v,version)
SELECT
  n,
  CAST(printf('%08x',n) || hex(zeroblob(124)) AS BLOB),
  0
FROM numbers
WHERE n<=65536
ORDER BY n;
COMMIT;
