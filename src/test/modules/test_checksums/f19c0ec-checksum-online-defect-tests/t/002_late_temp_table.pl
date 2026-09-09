# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify the documented cutoff for temporary tables during online checksum
# enablement.  A table created after the cluster enters inprogress-on receives
# checksums and must not delay completion.  At present each database worker
# takes its initial-temp snapshot only when that worker starts, so a late table
# in a database not yet visited is mistakenly treated as old.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('late_temp');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf',
	"shared_buffers = '16MB'\nmax_worker_processes = 16\n");
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE late_a');
$node->safe_psql('postgres', 'CREATE DATABASE late_b');

# Put a pre-existing temporary table in every connectable database that might
# be visited first.  Whichever worker reaches one first will wait there, giving
# us a deterministic window after the command starts but before a later
# database worker starts.
my %old_session;
for my $dbname (qw(postgres template1 late_a late_b))
{
	my $session = $node->background_psql($dbname);
	$session->query_safe('CREATE TEMP TABLE old_temp(i int)');
	$old_session{$dbname} = $session;
}

is($node->safe_psql('postgres', 'SHOW data_checksums'), 'off',
	'checksums start disabled');
$node->safe_psql('postgres', 'SELECT pg_enable_data_checksums()');

$node->wait_for_event('datachecksums worker',
	'ChecksumEnableTemptableWait');

my $blocked_db = $node->safe_psql(
	'postgres', q{
	SELECT datname
	FROM pg_stat_activity
	WHERE backend_type = 'datachecksums worker'
	  AND wait_event = 'ChecksumEnableTemptableWait'});
ok(exists $old_session{$blocked_db},
	"an intentionally held database blocked the first worker ($blocked_db)");
is($node->safe_psql('postgres', 'SHOW data_checksums'), 'inprogress-on',
	'enablement has started before the late table is created');

# Pick a database whose worker cannot have run yet: had either candidate run,
# its still-live old_temp would have blocked that earlier worker instead.
my $target = $blocked_db eq 'late_a' ? 'late_b' : 'late_a';

# Replace the target's pre-command temp table with one created strictly after
# the command entered inprogress-on, while an earlier database remains held.
$old_session{$target}->quit;
delete $old_session{$target};
my $late_session = $node->background_psql($target);
$late_session->query_safe('CREATE TEMP TABLE late_temp(i int)');
pass("created a temporary table in $target after enablement started");

# Release every genuinely pre-existing temp table.  The late table remains.
for my $session (values %old_session)
{
	$session->quit;
}
%old_session = ();

# Stop as soon as either the correct outcome occurs or the target worker is
# observed waiting for the late table.  This avoids a long timeout on the
# defective branch while retaining a bounded fallback for slow machines.
my $outcome = 'timeout';
for my $attempt (1 .. 10 * $PostgreSQL::Test::Utils::timeout_default)
{
	my $state = $node->safe_psql('postgres', 'SHOW data_checksums');
	if ($state eq 'on')
	{
		$outcome = 'on';
		last;
	}

	my $waiting = $node->safe_psql(
		'postgres', qq{
		SELECT count(*) > 0
		FROM pg_stat_activity
		WHERE backend_type = 'datachecksums worker'
		  AND datname = '$target'
		  AND wait_event = 'ChecksumEnableTemptableWait'});
	if ($waiting eq 't')
	{
		$outcome = 'blocked-by-late-temp';
		last;
	}

	PostgreSQL::Test::Utils::usleep(100_000);
}

# Drop the late table and allow the defective branch to finish, so teardown is
# clean and the test does not leave a live checksum worker behind.
$late_session->quit;
$node->poll_query_until('postgres', 'SHOW data_checksums', 'on')
  or die 'checksum enablement did not finish during cleanup';
$node->stop;

diag("online checksum enable outcome with late temp table: $outcome");
is($outcome, 'on',
	'a temporary table created after enablement starts does not block completion');

done_testing();
