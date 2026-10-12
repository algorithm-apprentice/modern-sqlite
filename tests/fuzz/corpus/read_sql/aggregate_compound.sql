SELECT DISTINCT nullable IS NULL,
       count(DISTINCT value<0),
       group_concat(label ORDER BY id DESC) FILTER (WHERE value>=0)
FROM model_rows
GROUP BY nullable IS NULL
UNION ALL
SELECT 2,
       count(),
       group_concat(DISTINCT nullable ORDER BY nullable DESC)
FROM model_rows
ORDER BY 1
LIMIT 2 OFFSET 1;
