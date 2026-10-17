-- Preserved wildcard namespace.
SELECT a.*,a.id,b.id FROM probe_rows a FULL JOIN key_rows b USING(id);
