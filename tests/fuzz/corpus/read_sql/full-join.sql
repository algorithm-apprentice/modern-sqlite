SELECT id,a.id,b.id,count(),count(a.id),count(b.id) FROM model_rows a NATURAL FULL JOIN model_rows b GROUP BY id ORDER BY id LIMIT 8;
