CREATE TABLE lineitem2_unindexed AS SELECT * FROM lineitem;

CREATE INDEX idx_lineitem_order ON lineitem(l_orderkey);
CREATE INDEX idx_orders_cust   ON orders(o_custkey);
CREATE INDEX idx_customer_nation ON customer(c_nationkey);
CLUSTER lineitem USING idx_lineitem_order;

CREATE OR REPLACE FUNCTION get_tax_rate()
RETURNS numeric
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN 0.07;
END;
$$;