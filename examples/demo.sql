-- A small, reproducible MiniDB session.
CREATE TABLE engineers (id INT PRIMARY KEY, name TEXT, team TEXT, tickets INT);
INSERT INTO engineers VALUES (101, 'Ada Lovelace', 'Platform', 8);
INSERT INTO engineers VALUES (102, 'Grace Hopper', 'Systems', 12);
INSERT INTO engineers VALUES (103, 'Alan Turing', 'Research', 5);
SELECT * FROM engineers;
EXPLAIN SELECT * FROM engineers WHERE id = 102;
SELECT name, team FROM engineers WHERE id = 102;
UPDATE engineers SET tickets = 13 WHERE id = 102;
DELETE FROM engineers WHERE id = 103;
SELECT COUNT(*) FROM engineers;
STATS;
COMPACT;
STATS;
