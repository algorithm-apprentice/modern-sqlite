SAVEPOINT outer;
INSERT INTO fuzz_target VALUES(2,'savepoint',2,x'02');
SAVEPOINT inner;
UPDATE fuzz_target SET value='inner' WHERE id=2;
ROLLBACK TO inner;
RELEASE inner;
ROLLBACK TO outer;
RELEASE outer;
