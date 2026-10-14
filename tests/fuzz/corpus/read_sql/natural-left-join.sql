SELECT id,a.id,b.id,count(),count(b.id) FROM model_rows a NATURAL LEFT JOIN model_rows b GROUP BY id ORDER BY id LIMIT 8;
