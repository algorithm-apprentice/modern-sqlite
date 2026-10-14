SELECT id,a.id,b.id FROM model_rows a FULL JOIN model_rows b USING(id) WHERE id<=8 UNION SELECT 99,NULL,99 ORDER BY 1
