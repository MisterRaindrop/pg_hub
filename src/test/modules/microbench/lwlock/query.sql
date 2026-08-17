-- lwlock micro-benchmark query

\pset format aligned

SELECT * FROM format_microbench(
	(SELECT array_agg(s ORDER BY s.id)
	 FROM bench_lwlock(:'n'::int8, :'rounds'::int8, true) AS s)
);
