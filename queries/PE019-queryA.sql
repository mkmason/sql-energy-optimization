-- PE019-queryA
EXPLAIN ANALYZE SELECT
    p_partkey,
    p_name,
    p_size
FROM part
WHERE p_partkey IN (
    SELECT ps_partkey
    FROM partsupp
    WHERE ps_availqty > 100
);
