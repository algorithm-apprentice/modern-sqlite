PRAGMA page_size=512;
PRAGMA auto_vacuum=NONE;
VACUUM;

CREATE TABLE items(
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL
);

INSERT INTO items VALUES
  (1, 'one'),
  (2, 'two');
