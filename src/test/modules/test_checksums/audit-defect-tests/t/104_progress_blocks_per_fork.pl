# Copyright (c) 2025, PostgreSQL Global Development Group
#
# Reproducer for a user-visible defect in the pg_stat_progress_data_checksums
# view added by commit f19c0ec ("Online enabling and disabling of data
# checksums").
#
# The documentation (doc/src/sgml/monitoring.sgml) states that the view's
# blocks_total / blocks_done columns count:
#     "The number of blocks in the current RELATION which will be processed"
#     "The number of blocks in the current RELATION which have been processed."
#
# But the worker tracks those counters PER FORK, not per relation:
# ProcessSingleRelationByOid() loops over every fork of a relation
# (datachecksum_state.c, the "for (ForkNumber fnum ...)" loop), and for each
# fork ProcessSingleRelationFork() unconditionally sets
#     PROGRESS_DATACHECKSUMS_BLOCKS_TOTAL = <blocks in THIS fork>
#     PROGRESS_DATACHECKSUMS_BLOCKS_DONE  = 0
# and then counts that fork's blocks up from 0.  relations_done is only bumped
# after the WHOLE relation finishes.
#
# So for any relation with more than one fork carrying blocks -- e.g. every
# vacuumed heap, which has main + fsm + vm forks -- while that single relation
# is "current":
#   * blocks_total takes several different values (one per fork), and never the
#     whole-relation block count the documentation promises; and
#   * blocks_done resets to 0 and thus moves BACKWARD at every fork boundary.
#
# This test drives an online enable under a heavy cost-delay throttle and
# samples the progress view, using the worker's pg_stat_activity string
# ("processing: public.big (<fork>, <n> blocks)") as ground truth for which
# relation/fork is current.  The two "# DEFECT:" assertions state the correct,
# per-relation behavior and therefore FAIL on this branch.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# ------------------------------------------------------------------ setup ---
# Start a cluster with data checksums OFF so we can enable them online.
my $node = PostgreSQL::Test::Cluster->new('csum_progress');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf',
	"shared_buffers = 16MB\n"
  . "max_worker_processes = 16\n");
$node->start;
is($node->safe_psql('postgres', 'SHOW data_checksums'),
	'off', 'setup: cluster started with data checksums off');

# A heap large enough to have a multi-block main fork, plus (after VACUUM) the
# fsm and vm forks, so a single relation spans several forks with blocks.
$node->safe_psql('postgres', qq{
	CREATE TABLE big AS
	  SELECT g AS id, repeat('x', 60) AS pad FROM generate_series(1, 10000) g;
	VACUUM (FREEZE, ANALYZE) big;
});

# Ground-truth per-fork sizes and the whole-relation total the docs promise.
my ($main_blk, $fsm_blk, $vm_blk) = split /\|/, $node->safe_psql('postgres', q{
	SELECT (pg_relation_size('big','main') / current_setting('block_size')::int)
	  ||'|'|| (pg_relation_size('big','fsm')  / current_setting('block_size')::int)
	  ||'|'|| (pg_relation_size('big','vm')   / current_setting('block_size')::int)
});
my $relation_total = $main_blk + $fsm_blk + $vm_blk;
note("big fork block counts: main=$main_blk fsm=$fsm_blk vm=$vm_blk; "
   . "whole-relation total = $relation_total");
cmp_ok($main_blk, '>=', 2, 'setup: big main fork has multiple blocks');
cmp_ok($fsm_blk + $vm_blk, '>=', 2,
	'setup: big has non-main forks (fsm/vm) with blocks');

# A read-only sampler.  It must NOT write any table: a writing transaction
# would acquire an xid that the launcher's WaitForAllTransactionsToFinish()
# could wait on, deadlocking against this very loop.  Reading the stat views +
# pg_sleep + building arrays acquires no xid, and pg_sleep/statement
# boundaries absorb the checksum procsignal barrier.  pg_stat_clear_snapshot()
# each iteration defeats the per-transaction caching of pg_stat_activity so we
# observe live changes.
$node->safe_psql('postgres', q{
CREATE FUNCTION sample_progress(target text, relation_total bigint)
RETURNS text AS $$
DECLARE
	q text; bt bigint; bd bigint; fork text;
	nsamp int := 0; seen boolean := false; post int := 0;
	forks text[] := '{}'; btots bigint[] := '{}';
	viol int := 0; backward int := 0; prev_bd bigint := NULL;
	bd_min bigint := NULL; bd_max bigint := NULL;
	start_ts timestamptz := clock_timestamp();
BEGIN
	LOOP
		PERFORM pg_stat_clear_snapshot();
		SELECT a.query, p.blocks_total, p.blocks_done
		  INTO q, bt, bd
		  FROM pg_stat_progress_data_checksums p
		  JOIN pg_stat_activity a ON a.pid = p.pid
		  WHERE p.datname = current_database()
		    AND a.query LIKE 'processing: ' || target || ' (%'
		  LIMIT 1;

		IF q IS NOT NULL THEN
			seen := true; post := 0; nsamp := nsamp + 1;
			fork := substring(q from '\(([a-z]+),');
			IF NOT (fork = ANY (forks)) THEN forks := forks || fork; END IF;
			IF NOT (bt = ANY (btots)) THEN btots := btots || bt; END IF;
			IF bt <> relation_total THEN viol := viol + 1; END IF;
			IF prev_bd IS NOT NULL AND bd < prev_bd THEN
				backward := backward + 1;
			END IF;
			prev_bd := bd;
			IF bd_min IS NULL OR bd < bd_min THEN bd_min := bd; END IF;
			IF bd_max IS NULL OR bd > bd_max THEN bd_max := bd; END IF;
		ELSIF seen THEN
			-- relation no longer current: it has finished being processed
			post := post + 1;
			prev_bd := NULL;
		END IF;

		EXIT WHEN seen AND post > 50;               -- target relation finished
		EXIT WHEN clock_timestamp() - start_ts > interval '90 seconds';  -- safety
		PERFORM pg_sleep(0.001);
	END LOOP;

	RETURN format(
		'nsamp=%s;forks=%s;distinct_forks=%s;distinct_btot=%s;viol=%s;backward=%s;bd_min=%s;bd_max=%s',
		nsamp, array_to_string(forks, ','),
		coalesce(array_length(forks, 1), 0),
		array_to_string(btots, ','), viol, backward,
		coalesce(bd_min::text,'NA'), coalesce(bd_max::text,'NA'));
END;
$$ LANGUAGE plpgsql;
});

# --------------------------------------------------------------- exercise ---
# Kick off the online enable (returns immediately; work happens in bg workers)
# under a heavy per-block throttle so the small forks stay observable.
$node->safe_psql('postgres', 'SELECT pg_enable_data_checksums(2, 1)');
pass('setup: online enable of data checksums started');

# Sample the progress view while public.big is the current relation.
my $res = $node->safe_psql('postgres',
	"SELECT sample_progress('public.big', $relation_total)",
	timeout => $PostgreSQL::Test::Utils::timeout_default);

my %r = map { split /=/, $_, 2 } split /;/, $res;
note("sampler result: $res");
note("distinct blocks_total observed while public.big was current: "
   . "{$r{distinct_btot}} -- documented value would be the constant "
   . "whole-relation total $relation_total");

# We must actually have watched the relation being processed, across >1 fork,
# or the demonstration below is inconclusive.  (Ground truth is the worker's
# own activity string, independent of the buggy counters.)
cmp_ok($r{nsamp}, '>', 0, 'setup: observed public.big being processed');
cmp_ok($r{distinct_forks}, '>=', 2,
	"setup: worker processed public.big across >=2 forks ($r{forks})");

# ---------------------------------------------------------- the defect(s) ---

# DEFECT: while a single relation is the "current relation", blocks_total must
# equal that relation's whole block count ($relation_total) -- per
# monitoring.sgml, "The number of blocks in the current relation".  On this
# branch blocks_total is instead the size of whichever fork is being processed,
# so every sample violates this.
is($r{viol}, 0,
	"blocks_total equals the current relation's total block count in every sample "
  . "(got $r{viol} of $r{nsamp} samples where blocks_total <> $relation_total; "
  . "observed blocks_total values {$r{distinct_btot}})");

# DEFECT: blocks_done reports progress through the current relation, so while a
# single relation stays current it must never move backward.  On this branch it
# is reset to 0 at every fork boundary, so it decreases at each boundary.
is($r{backward}, 0,
	"blocks_done never moves backward while public.big stays the current relation "
  . "(observed $r{backward} backward step(s); blocks_done ranged $r{bd_min}..$r{bd_max})");

$node->stop;
done_testing();
