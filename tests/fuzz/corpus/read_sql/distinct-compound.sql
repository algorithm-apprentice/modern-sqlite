VALUES(NULL),(1),(1.0)
UNION SELECT DISTINCT nullable FROM model_rows
EXCEPT SELECT nullable FROM model_rows WHERE id<8;
