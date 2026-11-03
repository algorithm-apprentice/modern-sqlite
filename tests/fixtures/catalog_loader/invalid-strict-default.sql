PRAGMA page_size=512;
CREATE TABLE invalid_strict(a ANY DEFAULT(5.00)) STRICT;
PRAGMA writable_schema=ON;
UPDATE sqlite_schema
SET sql='CREATE TABLE invalid_strict(a NUMERIC DEFAULT(5.00)) STRICT'
WHERE name='invalid_strict';
PRAGMA schema_version=2;
