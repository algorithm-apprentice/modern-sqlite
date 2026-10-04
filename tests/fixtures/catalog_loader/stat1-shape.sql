.dbconfig defensive off
PRAGMA page_size=512;
VACUUM;
CREATE TABLE stat_target(value TEXT);
CREATE INDEX stat_target_index ON stat_target(value);
CREATE TABLE temporary_stat1(extra, stat, tbl, idx);
INSERT INTO temporary_stat1
VALUES('ignored', '100 10 sz=2147483648', 'stat_target', 'stat_target_index');
PRAGMA writable_schema=ON;
UPDATE sqlite_schema
SET name = 'sqlite_stat1',
    tbl_name = 'sqlite_stat1',
    sql = 'CREATE TABLE sqlite_stat1(extra, stat, tbl, idx)'
WHERE name = 'temporary_stat1';
PRAGMA schema_version=2;
