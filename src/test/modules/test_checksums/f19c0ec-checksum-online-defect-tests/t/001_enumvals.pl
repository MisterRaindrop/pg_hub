# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify that pg_settings gives internally consistent metadata for the
# data_checksums setting.  Commit f19c0ec converted this GUC from bool to enum,
# but marked every enum entry hidden.  Consequently enumvals is {""}, even
# though the setting is one of on, off, inprogress-on, or inprogress-off.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('enumvals');
$node->init(no_data_checksums => 1);
$node->start;

my $metadata = $node->safe_psql(
	'postgres', q{
	SELECT format('type=%s setting=%s enumvals=%s',
	              vartype, setting, coalesce(enumvals::text, 'NULL'))
	FROM pg_settings
	WHERE name = 'data_checksums'});
note("data_checksums metadata: $metadata");

# pg_settings documents enumvals as the allowed values for enum parameters.
# If data_checksums remains an enum, containment is tolerant of future states
# while requiring the four states introduced by f19c0ec, and membership makes
# its metadata self-consistent.  Converting this read-only status setting to a
# non-enum type is also a valid fix, in which case enumvals must be NULL and
# the displayed status must still be one of the four defined states.
is(
	$node->safe_psql(
		'postgres', q{
		SELECT CASE WHEN vartype = 'enum' THEN
		         enumvals @> ARRAY[
		           'on', 'off', 'inprogress-on', 'inprogress-off'
		         ]::text[]
		         AND setting = ANY(enumvals)
		       ELSE enumvals IS NULL
		         AND setting = ANY(ARRAY[
		           'on', 'off', 'inprogress-on', 'inprogress-off'
		         ]::text[])
		       END
		FROM pg_settings
		WHERE name = 'data_checksums'}),
	't',
	'data_checksums exposes consistent type metadata through pg_settings');

$node->stop;
done_testing();
