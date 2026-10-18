SELECT sum((SELECT o.value)) AS x
FROM model_rows o
HAVING sum((SELECT o.id))>0
UNION ALL
SELECT 0
ORDER BY x;
