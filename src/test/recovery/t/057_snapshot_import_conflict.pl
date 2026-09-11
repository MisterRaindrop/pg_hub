# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify that standby recovery prevents snapshot imports from creating new
# conflicts after it has collected the VXIDs that conflict with a cleanup WAL
# record.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;

if (!$primary->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$primary->safe_psql(
	'postgres', q[
CREATE EXTENSION injection_points;
CREATE TABLE t AS SELECT generate_series(1, 100) AS id;
]);

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	'max_standby_streaming_delay = -1');
$standby->start;

# Register an old xmin and export its snapshot on the standby.
my $exporter =
  $standby->background_psql('postgres', on_error_stop => 0);
$exporter->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
is($exporter->query_safe('SELECT count(*) FROM t'), 100,
	'exporter sees all rows');
my $old_snapshot = $exporter->query_safe('SELECT pg_export_snapshot()');

$standby->safe_psql(
	'postgres', q[
SELECT injection_points_attach('recovery-conflict-snapshot-scan-complete', 'wait');
SELECT injection_points_attach('recovery-conflict-snapshot-resolved', 'wait');
]);

# Generate a cleanup record whose replay conflicts with the old snapshot.
$primary->safe_psql(
	'postgres', q[
DELETE FROM t;
VACUUM t;
]);

# Recovery has completed its conflict scan, but has not started waiting for
# the exporter yet.
$standby->wait_for_event('startup',
	'recovery-conflict-snapshot-scan-complete');

my ($stdout, $stderr);
my $result = $standby->psql(
	'postgres',
	"BEGIN ISOLATION LEVEL REPEATABLE READ; "
	  . "SET TRANSACTION SNAPSHOT '$old_snapshot';",
	stdout => \$stdout,
	stderr => \$stderr);
isnt($result, 0, 'cannot import a snapshot from a tracked source');

# Let recovery start waiting, then end the conflicting VXID.  Recovery pauses
# after resolving it, before it can replay the cleanup record.
$standby->safe_psql(
	'postgres', q[
SELECT injection_points_detach('recovery-conflict-snapshot-scan-complete');
SELECT injection_points_wakeup('recovery-conflict-snapshot-scan-complete');
]);
$exporter->query_safe('COMMIT');
$standby->wait_for_event('startup',
	'recovery-conflict-snapshot-resolved');

# Reuse the same backend for a new transaction.  Snapshot import must be
# allowed again once startup has resolved the old VXID.
$exporter->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $new_snapshot = $exporter->query_safe('SELECT pg_export_snapshot()');
$result = $standby->psql(
	'postgres',
	"BEGIN ISOLATION LEVEL REPEATABLE READ; "
	  . "SET TRANSACTION SNAPSHOT '$new_snapshot'; SELECT count(*) FROM t;",
	stdout => \$stdout,
	stderr => \$stderr);
is($result, 0, 'can import from the source after its tracked VXID ends');
$stdout =~ s/^\s+|\s+$//g;
is($stdout, '0', 'new snapshot sees the replayed delete');

$standby->safe_psql(
	'postgres', q[
SELECT injection_points_detach('recovery-conflict-snapshot-resolved');
SELECT injection_points_wakeup('recovery-conflict-snapshot-resolved');
]);
$exporter->query_safe('COMMIT');
$exporter->quit;

$primary->wait_for_replay_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'), 0,
	'standby replays the cleanup after the conflict ends');

$standby->stop;
$primary->stop;

done_testing();
