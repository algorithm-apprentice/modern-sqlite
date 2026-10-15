PRAGMA page_size=512;
PRAGMA auto_vacuum=NONE;
PRAGMA journal_mode=DELETE;
CREATE TABLE key_rows(
  id INTEGER PRIMARY KEY,
  text_key TEXT COLLATE NOCASE,
  numeric_key NUMERIC,
  real_key REAL,
  payload TEXT
);
INSERT INTO key_rows VALUES
  (1,'1',1,1.0,'one'),
  (2,'01',1,1.0,'zero-one'),
  (3,'0001',1,1.0,'zeros-one'),
  (4,'alpha',2,2.5,'alpha'),
  (5,'ALPHA',3,3.5,'upper-alpha'),
  (6,NULL,NULL,4.5,'null-one'),
  (7,NULL,4,5.5,'null-two'),
  (8,'beta',NULL,6.5,'beta-null'),
  (9,'beta',5,NULL,'beta-five'),
  (10,'same',6,8.5,'same-one'),
  (11,'same',6,8.5,'same-two'),
  (12,NULL,NULL,NULL,'null-three');
ALTER TABLE key_rows ADD COLUMN default_text TEXT DEFAULT 'fallback';
CREATE INDEX keys_text_num ON key_rows(text_key COLLATE NOCASE DESC,numeric_key);
CREATE INDEX keys_num_real ON key_rows(numeric_key DESC,real_key);
CREATE INDEX keys_binary_text ON key_rows(text_key COLLATE BINARY);
CREATE INDEX keys_default_num ON key_rows(default_text,numeric_key);
CREATE TABLE probe_rows(
  id INTEGER PRIMARY KEY,
  text_key TEXT,
  numeric_key NUMERIC,
  raw_key,
  blob_key BLOB
);
INSERT INTO probe_rows VALUES
  (1,'1',1,1,1),
  (2,'ALPHA',2,'ALPHA','ALPHA'),
  (3,NULL,NULL,NULL,NULL),
  (4,'beta',5,'beta','beta'),
  (5,'missing',99,'missing','missing'),
  (6,'fallback',99,NULL,NULL);
CREATE TABLE empty_probe_rows(id INTEGER PRIMARY KEY,text_key TEXT);
CREATE TABLE empty_key_rows(
  id INTEGER PRIMARY KEY,
  text_key TEXT COLLATE NOCASE,
  numeric_key NUMERIC
);
CREATE INDEX empty_keys_text_num
  ON empty_key_rows(text_key COLLATE NOCASE DESC,numeric_key);
VACUUM;
