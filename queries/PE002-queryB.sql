-- PE002-queryB
EXPLAIN ANALYZE SELECT
    p_partkey,
    p_name,
    p_size
FROM public.part
WHERE p_size > 10;
