/* Automatic equality prefixes, duplicate NULL keys, and two live lookup sources. */
SELECT a.id,b.id,c.id,b.label,c.label FROM model_rows a
LEFT JOIN model_rows b ON b.nullable IS a.nullable
LEFT JOIN model_rows c ON c.value=b.value
WHERE a.id<=4 ORDER BY a.id,b.id,c.id LIMIT 17;
