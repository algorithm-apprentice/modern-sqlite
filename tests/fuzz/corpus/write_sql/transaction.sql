BEGIN IMMEDIATE;
INSERT INTO fuzz_target VALUES(2,'transaction',2,x'02');
UPDATE fuzz_target SET value='committed' WHERE id=2;
COMMIT;
SELECT id,value FROM fuzz_target;
