SELECT id,a.*,b.id FROM model_rows a LEFT JOIN items b USING(id) WHERE id<=8 ORDER BY id;
