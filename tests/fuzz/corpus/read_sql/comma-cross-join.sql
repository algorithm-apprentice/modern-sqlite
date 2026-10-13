SELECT DISTINCT a.id,b.id,a.nullable+b.nullable FROM model_rows a CROSS JOIN model_rows b WHERE a.id<=3 AND b.id<=4 ORDER BY 1 DESC,2 LIMIT 4 OFFSET 1;
