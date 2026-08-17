CREATE FUNCTION bench_lwlock(
	IN n int8,
	IN rounds int8 DEFAULT 1,
	IN random bool DEFAULT true
)
RETURNS SETOF microbench_sample
AS 'MODULE_PATHNAME', 'bench_lwlock'
LANGUAGE C;

REVOKE ALL ON FUNCTION bench_lwlock(int8, int8, bool) FROM PUBLIC;
