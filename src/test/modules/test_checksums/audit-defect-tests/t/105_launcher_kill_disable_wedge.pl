# 105_launcher_kill_disable_wedge.pl
#
# Commit f19c0ec "Online enabling and disabling of data checksums" installs
# launcher_exit() (src/backend/postmaster/datachecksum_state.c) as the
# on_shmem_exit cleanup that is meant to leave a consistent checksum state
# whenever the "datachecksums launcher" process exits.  It only heals an
# interrupted *enable*:
#
#     if (DataChecksumsInProgressOn())
#         SetDataChecksumsOff();
#
# There is no branch for an interrupted *disable*, and no DataChecksumsInProgressOff()
# helper exists at all.  SetDataChecksumsOff() (xlog.c) disables in two committed
# steps separated by a forced RequestCheckpoint(CHECKPOINT_WAIT) plus a
# WaitForProcSignalBarrier: it first persists ControlFile->data_checksum_version =
# PG_DATA_CHECKSUM_INPROGRESS_OFF, then (only after the checkpoint completes)
# PG_DATA_CHECKSUM_OFF.  If the launcher is terminated (pg_terminate_backend ->
# SIGTERM -> die -> proc_exit -> launcher_exit) during that window, launcher_exit()
# runs while the persisted state is 'inprogress-off'; DataChecksumsInProgressOn()
# is false, so nothing finishes the transition.  bgw_restart_time is
# BGW_NEVER_RESTART, so no launcher is relaunched: the cluster is wedged in
# 'inprogress-off' with no process left to advance it.  (A server restart would
# self-heal it via StartupXLOG's end-of-recovery handling, but a live-cluster
# SIGTERM never reaches that path.)
#
# This test contrasts the two cases using the SAME kill:
#   * launcher killed mid-ENABLE  (state 'inprogress-on')  -> self-heals to 'off'
#   * launcher killed mid-DISABLE (state 'inprogress-off') -> should also reach 'off'
# The first (control) passes on this branch and on a fixed branch; the second is
# the defect and fails on this branch, where the state stays 'inprogress-off'.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('csum_kill_disable');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', <<'EOF');
shared_buffers = 16MB
max_worker_processes = 16
checkpoint_timeout = 1h
log_min_messages = warning
shared_preload_libraries = 'injection_points'
EOF
$node->start;

# Current 'data_checksums' setting (what a client sees via SHOW / pg_settings).
sub cstate
{
	return $node->safe_psql('postgres',
		"SELECT setting FROM pg_settings WHERE name = 'data_checksums';");
}

# pid of the datachecksums launcher, or '' if none is running.
sub launcher_pid
{
	return $node->safe_psql('postgres',
		"SELECT pid FROM pg_stat_activity "
	  . "WHERE backend_type = 'datachecksums launcher' LIMIT 1;");
}

# Block until the launcher has fully exited (its on_shmem_exit cleanup, i.e.
# launcher_exit(), has therefore already run to completion).
sub wait_no_launcher
{
	$node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity "
	  . "WHERE backend_type = 'datachecksums launcher';")
	  or die "datachecksums launcher never exited";
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');
# A little data so the enable worker has real work to do before it parks.
$node->safe_psql('postgres',
	'CREATE TABLE t AS SELECT generate_series(1, 10000) AS a;');

is(cstate(), 'off', 'setup: cluster initialized with checksums off');

#############################################################################
# CONTROL -- terminate the launcher mid-ENABLE (state 'inprogress-on').
#
# launcher_exit()'s one existing branch (DataChecksumsInProgressOn ->
# SetDataChecksumsOff) turns this back to 'off'.  This passes on this branch
# and would pass on a fixed branch too, isolating the disable-only regression.
#############################################################################

# Hold the enable worker inside its temp-table wait so the cluster sits in
# 'inprogress-on' with the launcher alive and waiting on the worker.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('datachecksumsworker-fake-temptable-wait','wait');");
$node->safe_psql('postgres', 'SELECT pg_enable_data_checksums(0, 100);');
$node->wait_for_event('datachecksums worker',
	'datachecksumsworker-fake-temptable-wait');
is(cstate(), 'inprogress-on',
	'control setup: enable is parked in inprogress-on');

my $lp = launcher_pid();
ok($lp ne '', "control setup: launcher is running (pid $lp)");

# Kill the launcher while state is 'inprogress-on'.  launcher_exit() signals the
# worker on its way out; terminate any worker explicitly too and confirm it is
# gone (its injection wait frees its slot on death), then drop the injection
# point so the follow-up enable is not affected by it.
$node->safe_psql('postgres', "SELECT pg_terminate_backend($lp);");
$node->safe_psql('postgres',
	"SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
  . "WHERE backend_type = 'datachecksums worker';");
$node->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity "
  . "WHERE backend_type = 'datachecksums worker';")
  or die "enable worker never exited";
wait_no_launcher();
$node->safe_psql('postgres',
	"SELECT injection_points_detach('datachecksumsworker-fake-temptable-wait');");

# Control result: the interrupted enable heals back to 'off' on its own.
my $after_enable_kill = $node->poll_query_until('postgres',
	"SELECT setting FROM pg_settings WHERE name = 'data_checksums';", 'off');
is($after_enable_kill, 1,
	'control: launcher killed mid-enable self-heals to off');
is(cstate(), 'off', 'control: state is off after the mid-enable kill');

#############################################################################
# DEFECT -- terminate the launcher mid-DISABLE (state 'inprogress-off').
#############################################################################

# Bring checksums fully ON first.
$node->safe_psql('postgres', 'SELECT pg_enable_data_checksums(0, 100);');
$node->poll_query_until('postgres',
	"SELECT setting FROM pg_settings WHERE name = 'data_checksums';", 'on')
  or die "enable never reached 'on'";
wait_no_launcher();
is(cstate(), 'on', 'defect setup: checksums fully enabled (on)');

# Block the forced checkpoint that SetDataChecksumsOff() runs *after* it has
# already persisted state 'inprogress-off'.  The checkpointer parks on this
# injection point, so the launcher blocks in RequestCheckpoint(CHECKPOINT_WAIT)
# with the persisted state at 'inprogress-off'.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('create-checkpoint-run','wait');");
$node->safe_psql('postgres', 'SELECT pg_disable_data_checksums();');

my $reached = $node->poll_query_until('postgres',
	"SELECT setting FROM pg_settings WHERE name = 'data_checksums';",
	'inprogress-off');
is($reached, 1, 'defect setup: disable is parked in inprogress-off');

my $lp2 = launcher_pid();
ok($lp2 ne '', "defect setup: launcher is running (pid $lp2)");

# Terminate the launcher while state is 'inprogress-off'.
$node->safe_psql('postgres', "SELECT pg_terminate_backend($lp2);");
wait_no_launcher();

# Release the orphaned checkpoint.  The checkpointer (not the launcher) is the
# process parked on this injection point, and it is still alive, so wakeup finds
# it.  After this the cluster is otherwise perfectly healthy.
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('create-checkpoint-run');");
$node->safe_psql('postgres',
	"SELECT injection_points_detach('create-checkpoint-run');");

# Give any would-be completion every chance: an ordinary checkpoint and a moment.
$node->safe_psql('postgres', 'CHECKPOINT;');
$node->safe_psql('postgres', 'SELECT pg_sleep(1);');

my $after_disable_kill = cstate();
diag("data_checksums after mid-disable launcher kill = '$after_disable_kill'");

# DEFECT: a launcher terminated mid-disable must still finish the transition to
# 'off', exactly as launcher_exit() finishes an interrupted enable.  The correct
# final state is 'off'.  On this branch launcher_exit() has no disable branch, so
# the cluster is left indefinitely in 'inprogress-off' with no process to advance
# it (recovered only by re-issuing pg_disable_data_checksums() or a server
# restart, both exercised below) -- this assertion fails.
is($after_disable_kill, 'off',
	'DEFECT: disable completes to off after launcher killed mid-inprogress-off');

# The remaining checks document, and pass under, the consequence of the defect
# on this branch: the wedge persists with no process left to advance it, and
# is only cleared by a manual re-issue of the disable (or a server restart).
# On a fixed branch the state would already be 'off' here and the re-issued
# disable would be a harmless no-op, so these still pass there.
my $n_launcher = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_stat_activity "
  . "WHERE backend_type = 'datachecksums launcher';");
is($n_launcher, '0', 'no launcher process remains to complete the disable');

$node->safe_psql('postgres', 'SELECT pg_disable_data_checksums();');
my $recovered = $node->poll_query_until('postgres',
	"SELECT setting FROM pg_settings WHERE name = 'data_checksums';", 'off');
wait_no_launcher();
is($recovered, 1,
	'a re-issued pg_disable_data_checksums() clears the wedge back to off');
is(cstate(), 'off', 'checksums are off again after the manual re-issue');

$node->stop;
done_testing();
