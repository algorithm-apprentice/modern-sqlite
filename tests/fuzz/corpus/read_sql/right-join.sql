SELECT id,a.id,b.id,count(a.id) FROM model_rows a NATURAL RIGHT JOIN model_rows b GROUP BY id ORDER BY id LIMIT 8;
