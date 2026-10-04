.dbconfig defensive off
PRAGMA page_size=512;
VACUUM;
CREATE TABLE duplicate_root(value TEXT UNIQUE);
PRAGMA writable_schema=ON;
INSERT INTO sqlite_schema(type, name, tbl_name, rootpage, sql)
SELECT type, name, tbl_name, rootpage, sql
FROM sqlite_schema
WHERE name = 'sqlite_autoindex_duplicate_root_1';
PRAGMA schema_version=2;
