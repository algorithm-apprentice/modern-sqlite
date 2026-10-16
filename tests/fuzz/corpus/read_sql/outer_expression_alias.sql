SELECT coalesce(o.nullable,o.value) AS outer_value FROM model_rows o WHERE o.id<=4 AND (SELECT (SELECT outer_value)) IS NOT NULL ORDER BY (SELECT outer_value),o.id;
