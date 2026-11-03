PRAGMA page_size=512;
CREATE TABLE invalid_boolean(a DEFAULT(0));
PRAGMA writable_schema=ON;
UPDATE sqlite_schema
SET sql='CREATE TABLE invalid_boolean(a DEFAULT("FALSE"))'
WHERE name='invalid_boolean';
PRAGMA schema_version=2;
