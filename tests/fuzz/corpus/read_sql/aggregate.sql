SELECT nullable IS NULL,
       count(),
       min(value),
       group_concat(label ORDER BY id DESC) FILTER (WHERE value>=0),
       count(DISTINCT nullable)
FROM model_rows
GROUP BY nullable IS NULL
HAVING count()>0
ORDER BY 1;
