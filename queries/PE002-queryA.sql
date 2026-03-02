-- PE002-queryA
EXPLAIN ANALYZE SELECT
    p_partkey,
    p_name,
    p_size
FROM part
WHERE p_size > 10;
