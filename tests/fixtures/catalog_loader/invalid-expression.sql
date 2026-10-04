.dbconfig defensive off
PRAGMA page_size=512;
VACUUM;
CREATE TABLE invalid_expression(a TEXT);
CREATE INDEX invalid_index ON invalid_expression(a);
PRAGMA writable_schema=ON;
UPDATE sqlite_schema
SET sql = 'CREATE INDEX invalid_index ON invalid_expression(missing)'
WHERE name = 'invalid_index';
PRAGMA schema_version=2;
