PRAGMA page_size = 512;
PRAGMA journal_mode = DELETE;

CREATE TABLE altered(
  id INTEGER PRIMARY KEY,
  existing TEXT
);

INSERT INTO altered VALUES(1, 'old');

ALTER TABLE altered ADD COLUMN text_default TEXT DEFAULT 'legacy';
ALTER TABLE altered ADD COLUMN real_default REAL DEFAULT 7;
ALTER TABLE altered ADD COLUMN null_default BLOB DEFAULT NULL;
ALTER TABLE altered ADD COLUMN blob_default BLOB DEFAULT X'0102';
ALTER TABLE altered ADD COLUMN negative_default INTEGER DEFAULT -5;

INSERT INTO altered(
  id,
  existing,
  text_default,
  real_default,
  null_default,
  blob_default,
  negative_default
) VALUES(2, 'new', NULL, NULL, NULL, NULL, NULL);

CREATE TABLE wr(
  key TEXT PRIMARY KEY,
  existing TEXT
) WITHOUT ROWID;

INSERT INTO wr VALUES('old', 'before');
ALTER TABLE wr ADD COLUMN added TEXT DEFAULT 'wr-default';
INSERT INTO wr VALUES('new', 'after', NULL);

PRAGMA integrity_check;
