PRAGMA page_size=512;
PRAGMA auto_vacuum=NONE;
PRAGMA journal_mode=DELETE;
CREATE TABLE wild_l(ID INT,"Key" TEXT COLLATE NOCASE,payload TEXT);
CREATE TABLE wild_r(id INTEGER PRIMARY KEY,"key" NUMERIC,extra TEXT);
INSERT INTO wild_l VALUES(1,'alpha','L1'),(2,'01','L2'),(3,NULL,'L3');
INSERT INTO wild_r VALUES(1,'ALPHA','R1'),(4,1,'R4'),(5,NULL,'R5');
CREATE TABLE only_id(id INTEGER);
INSERT INTO only_id VALUES(1),(7);
