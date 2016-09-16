CREATE TABLE excluded_startup_keys (key_name text primary key);

INSERT INTO excluded_startup_keys
VALUES
('pg_version_num'),('pg_version'),('pg_catversion'),('binary.basetypes_major_version'),('binary.integer_datetimes'),('binary.bigendian'),('binary.maxalign'),('binary.binary_pg_version'),('sizeof_int'),('sizeof_long'),('sizeof_datum'),('pglogical_output_version'),('walsender_pid');

CREATE UNLOGGED TABLE json_decoding_output(ch jsonb, rn integer);

CREATE OR REPLACE FUNCTION get_startup_params()
RETURNS TABLE ("key" text, "value" jsonb)
LANGUAGE sql
AS $$
SELECT key, value
FROM json_decoding_output
CROSS JOIN LATERAL jsonb_each(ch -> 'params')
WHERE rn = 1
  AND key NOT IN (SELECT * FROM excluded_startup_keys)
  AND ch ->> 'action' = 'S'
ORDER BY key;
$$;

CREATE OR REPLACE FUNCTION get_queued_data()
RETURNS TABLE (data jsonb)
LANGUAGE sql
AS $$
SELECT ch
FROM json_decoding_output
WHERE rn > 1
ORDER BY rn ASC;
$$;
