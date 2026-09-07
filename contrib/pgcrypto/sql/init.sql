--
-- init pgcrypto
--

CREATE EXTENSION pgcrypto;

-- check that the compatibility function agrees with core
select public.fips_mode() = pg_catalog.fips_mode() AS same_fips_mode;

-- check error handling
select gen_salt('foo');
select digest('foo', 'foo');
select hmac('foo', 'foo', 'foo');
select encrypt('foo', 'foo', 'foo');
