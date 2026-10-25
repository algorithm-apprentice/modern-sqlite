SELECT id,id IN (1,2,NULL),id NOT IN (1,2,3) FROM model_rows WHERE id<=4;
SELECT nullable IN (),nullable NOT IN () FROM model_rows WHERE id<=4;
SELECT label IN ('A' COLLATE NOCASE,'b','c') FROM model_rows WHERE id<=4;
SELECT (SELECT o.nullable IN (1,2,3)) FROM model_rows o WHERE o.id<=4;
SELECT value IN (1,2,3) AS matched FROM model_rows WHERE id<=4 AND matched ORDER BY matched;
SELECT value IN (1,2,3),count() FROM model_rows WHERE id<=4 GROUP BY 1 ORDER BY 1;
SELECT 1 IN (1,abs(-9223372036854775808));
SELECT 1 IN (1,0 AND id,abs(-9223372036854775808)) FROM model_rows;
SELECT 1 IN (1,0_0 AND id,abs(-9223372036854775808)) FROM model_rows;
