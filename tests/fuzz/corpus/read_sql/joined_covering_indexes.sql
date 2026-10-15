-- ADR-0085: route this seed and its generated family to the indexed fixture.
SELECT p.id,b.id,c.id
FROM probe_rows p FULL JOIN key_rows b ON b.text_key IS p.text_key
FULL JOIN key_rows c ON c.text_key=b.text_key AND c.numeric_key=b.numeric_key
ORDER BY p.id,b.id,c.id LIMIT 12 OFFSET 1
