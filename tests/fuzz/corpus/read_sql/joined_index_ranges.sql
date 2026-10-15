-- Joined next-term ranges retain their end keys across the shared FULL producer.
SELECT p.id,count(b.payload),group_concat(b.payload ORDER BY b.id DESC)
FROM probe_rows p FULL JOIN key_rows b
ON b.text_key IS p.text_key
AND b.numeric_key>=p.numeric_key AND b.numeric_key<=5
GROUP BY p.id ORDER BY p.id LIMIT 12 OFFSET 1
