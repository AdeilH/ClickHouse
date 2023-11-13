DROP TABLE IF EXISTS test;

CREATE TABLE test ( col1 Int64, dt Date ) ENGINE = MergeTree PARTITION BY dt ORDER BY tuple();

INSERT INTO test FORMAT Values (1, today());

ALTER TABLE test ADD COLUMN col2 String;

ALTER TABLE test ADD INDEX i1 (col1, col2) TYPE set(100) GRANULARITY 1;

ALTER TABLE test MATERIALIZE INDEX i1;

ALTER TABLE test ADD COLUMN col3 String;

ALTER TABLE test DROP COLUMN col3;

DROP TABLE IF EXISTS test;

CREATE TABLE test ( col1 Int64, dt Date ) ENGINE = MergeTree PARTITION BY dt ORDER BY tuple();

INSERT INTO test FORMAT Values (1, today());

ALTER TABLE test ADD COLUMN col2 String;

ALTER TABLE test ADD PROJECTION p1 ( SELECT col2, sum(col1) GROUP BY col2 );

ALTER TABLE test MATERIALIZE PROJECTION p1;

ALTER TABLE test ADD COLUMN col3 String;

ALTER TABLE test DROP COLUMN col3;
