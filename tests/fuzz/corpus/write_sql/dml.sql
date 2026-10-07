INSERT INTO fuzz_target VALUES(2,'two',2,x'02');
UPDATE fuzz_target SET id=id+10,value=value||'x' WHERE score>=1;
DELETE FROM fuzz_target WHERE id=11;
SELECT id,value,score,payload FROM fuzz_target;
