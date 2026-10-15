-- Noncovering joined equality and complete table-backed FULL replay.
SELECT p.id,b.id,b.payload,b.default_text,typeof(b.real_key),b.real_key
FROM probe_rows p FULL JOIN key_rows b ON b.text_key IS p.text_key
ORDER BY p.id,b.id LIMIT 12 OFFSET 1
