PRAGMA page_size=4096;
PRAGMA journal_mode=DELETE;
PRAGMA synchronous=FULL;
PRAGMA auto_vacuum=NONE;
PRAGMA encoding='UTF-8';
PRAGMA application_id=0;
PRAGMA user_version=1;

CREATE TABLE kv(
  k INTEGER PRIMARY KEY,
  v BLOB NOT NULL,
  version INTEGER NOT NULL
);
