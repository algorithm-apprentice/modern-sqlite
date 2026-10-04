.dbconfig defensive off
PRAGMA page_size=512;
VACUUM;
CREATE TABLE compatibility(
  a TEXT COLLATE NOCASE CHECK(rowid > 0),
  b DEFAULT(1),
  c DEFAULT(abs()),
  UNIQUE((a))
);
CREATE INDEX application_index ON compatibility(a);
CREATE INDEX parenthesized_index
ON compatibility((a COLLATE RTRIM))
WHERE rowid > 0;
PRAGMA writable_schema=ON;
UPDATE sqlite_schema
SET sql = 'CREATE TABLE compatibility(
  a TEXT COLLATE NOCASE CHECK(rowid > 0),
  b DEFAULT(?),
  c DEFAULT(abs()),
  UNIQUE((a))
)'
WHERE name = 'compatibility';
UPDATE sqlite_schema
SET sql = 'CREATE INDEX application_index ON compatibility(application_function(a))'
WHERE name = 'application_index';
PRAGMA schema_version=2;
