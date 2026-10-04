.dbconfig defensive off
PRAGMA page_size=512;
VACUUM;
CREATE TABLE invalid_function(a TEXT);
CREATE INDEX invalid_function_index ON invalid_function(a);
PRAGMA writable_schema=ON;
UPDATE sqlite_schema
SET sql = 'CREATE INDEX invalid_function_index ON invalid_function(lower(a, a))'
WHERE name = 'invalid_function_index';
PRAGMA schema_version=2;
