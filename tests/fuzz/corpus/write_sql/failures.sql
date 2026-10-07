INSERT INTO fuzz_target VALUES(1,'duplicate',1,x'01');
INSERT INTO fuzz_target(id,value) VALUES(3,NULL);
INSERT INTO fuzz_target(id,value) VALUES(4.5,'fractional');
COMMIT;
SELECT id FROM fuzz_target;
