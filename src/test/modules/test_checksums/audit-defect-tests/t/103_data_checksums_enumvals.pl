# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Reproducer for a defect introduced by commit f19c0ec ("Online enabling and
# disabling of data checksums").  That commit turned the "data_checksums" GUC
# from a bool into an enum GUC, but its option table
# (data_checksums_options[] in src/backend/utils/misc/guc_tables.c) marks ALL
# FOUR values -- "on"/"off"/"inprogress-on"/"inprogress-off" -- as hidden.
#
# Because every entry is hidden, config_enum_get_options() (guc.c) appends no
# value names, so when pg_settings builds the "enumvals" column (guc_funcs.c)
# it produces the bogus one-element array {""} -- an array whose single
# element is the empty string -- in EVERY checksum state.  A correct enum GUC
# reports either NULL (as the pre-f19c0ec bool data_checksums did) or the list
# of valid values (as wal_level and the sibling read-only preset enum
# huge_pages_status do).
#
# The universal, machine-checkable invariant that every other enum GUC in the
# tree satisfies is:
#
#     enumvals IS NULL OR setting = ANY(enumvals)
#
# i.e. the current value is always a member of its own advertised value list
# (or the list is absent).  data_checksums violates it because its setting
# ('off', 'on', ...) is never a member of {""}.  The failing assertions below
# are written to expect the CORRECT behavior and therefore fail on this
# branch; that failure is the demonstration.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Start a cluster with data checksums OFF, so that the defect can be observed
# both in the initial "off" state and across an online enable to "on".
my $node = PostgreSQL::Test::Cluster->new('csum_enumvals');
$node->init(no_data_checksums => 1);
$node->append_conf(
	'postgresql.conf', qq{
shared_buffers = 16MB
max_worker_processes = 16
});
$node->start;

# Convenience: fetch a single scalar from pg_settings for data_checksums.
sub dc
{
	my ($expr) = @_;
	return $node->safe_psql('postgres',
		"SELECT $expr FROM pg_settings WHERE name = 'data_checksums'");
}

# Emit the full enumvals picture as a diagnostic so failures are self-evident.
sub diag_enumvals
{
	my ($label) = @_;
	my $row = $node->safe_psql(
		'postgres', qq{
		SELECT format('setting=%s enumvals=%s in_enumvals=%s',
		              setting,
		              coalesce(enumvals::text, 'NULL'),
		              coalesce((setting = ANY(enumvals))::text, 'NULL'))
		FROM pg_settings WHERE name = 'data_checksums'});
	diag("data_checksums [$label]: $row");
}

# ---------------------------------------------------------------------------
# Setup / context assertions (these PASS): confirm data_checksums is an enum
# GUC and that the equivalent enum GUCs elsewhere in the tree behave correctly.
# This is what makes data_checksums' behavior a defect rather than a choice.
# ---------------------------------------------------------------------------

is(dc('vartype'), 'enum',
	'data_checksums is an enum GUC (context for the defect)');

# Every other enum GUC satisfies "current setting is a member of enumvals".
# huge_pages_status is the closest sibling: a read-only (PGC_INTERNAL) preset
# enum GUC, exactly like data_checksums.
for my $g (qw(huge_pages_status wal_level))
{
	my $ok = $node->safe_psql('postgres',
		"SELECT setting = ANY(enumvals) FROM pg_settings WHERE name = '$g'");
	is($ok, 't',
		"sibling enum GUC $g: current setting is a member of its enumvals");
}

diag_enumvals('checksums off');

# ---------------------------------------------------------------------------
# Defect demonstration in the OFF state (these FAIL on this branch).
# ---------------------------------------------------------------------------

# DEFECT: pg_settings.enumvals for an enum GUC must be NULL or list the valid
# values, and the current setting must be one of them -- so
# "enumvals IS NULL OR setting = ANY(enumvals)" must return 't', exactly as it
# does for wal_level, huge_pages_status and every other enum GUC.  On this
# branch enumvals is {""} and setting is 'off', so the query returns 'f'.
is(dc('enumvals IS NULL OR setting = ANY(enumvals)'),
	't',
	'data_checksums (off): setting is a member of enumvals, or enumvals is NULL'
);

# DEFECT: enumvals must not be the phantom single-empty-string array {""}.  A
# correct implementation reports NULL (like the pre-f19c0ec bool GUC) or the
# real values {on,off,inprogress-on,inprogress-off}; both are DISTINCT FROM
# {""}.  On this branch enumvals is exactly {""}, so this returns 'f'.
# (IS DISTINCT FROM is NULL-safe, so a NULL-enumvals fix still passes.)
is(dc(q{enumvals IS DISTINCT FROM '{""}'::text[]}),
	't',
	'data_checksums (off): enumvals is not the phantom empty-string array {""}'
);

# ---------------------------------------------------------------------------
# Online-enable the checksums; the enable itself is setup and PASSES.  Then
# confirm the defect persists in the ON state.
# ---------------------------------------------------------------------------

$node->safe_psql('postgres', 'SELECT pg_enable_data_checksums()');
$node->poll_query_until('postgres',
	"SELECT setting = 'on' FROM pg_settings WHERE name = 'data_checksums'")
  or die "timed out waiting for data checksums to be enabled";
pass('online enable of data checksums reached the "on" state (setup)');

diag_enumvals('checksums on');

# DEFECT: the same invariant must hold in the 'on' state -- the setting is now
# 'on', which must be a member of enumvals.  On this branch enumvals is still
# {""}, so the setting is never a member of its own value list in any state,
# and this returns 'f'.
is(dc('enumvals IS NULL OR setting = ANY(enumvals)'),
	't',
	'data_checksums (on): setting is a member of enumvals, or enumvals is NULL'
);

$node->stop;

done_testing();
