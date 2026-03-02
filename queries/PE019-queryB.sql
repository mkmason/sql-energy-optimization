-- PE019-queryB
EXPLAIN ANALYZE SELECT
    p.p_partkey,
    p.p_name,
    p.p_size
FROM part p
WHERE EXISTS (
    SELECT 1
    FROM partsupp ps
    WHERE ps.ps_partkey = p.p_partkey
      AND ps.ps_availqty > 100
);
