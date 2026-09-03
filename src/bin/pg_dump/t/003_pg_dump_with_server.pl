
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $node = PostgreSQL::Test::Cluster->new('main');
my $port = $node->port;

$node->init;
$node->start;

#########################################
# Verify that dumping foreign data includes only foreign tables of
# matching servers

$node->safe_psql('postgres', "CREATE FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s0 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s1 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s2 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE FOREIGN TABLE t0 (a int) SERVER s0");
$node->safe_psql('postgres', "CREATE FOREIGN TABLE t1 (a int) SERVER s1");

command_fails_like(
	[
		"pg_dump",
		'--port' => $port,
		'--include-foreign-data' => 's0',
		'postgres'
	],
	qr/foreign-data wrapper \"dummy\" has no handler\r?\npg_dump: detail: Query was: .*t0/,
	"correctly fails to dump a foreign table from a dummy FDW");

command_ok(
	[
		"pg_dump",
		'--port' => $port,
		'--data-only',
		'--include-foreign-data' => 's2',
		'postgres'
	],
	"dump foreign server with no tables");

#########################################
# Verify that --binary-upgrade lists an extension's required extensions in
# name order.  pg_dump reads the requires list out of pg_depend, which
# returns those rows in an order derived from the required extensions'
# OIDs; without an explicit sort, two databases holding the same extensions
# dump differently depending on the order the extensions were created in.

mkdir "$tempdir/extension"
  or die "could not create directory \"$tempdir/extension\": $!";
foreach my $ext ('dump_test_ext_a', 'dump_test_ext_b', 'dump_test_ext_c')
{
	open my $cf, '>', "$tempdir/extension/$ext.control"
	  or die "could not create control file for $ext: $!";
	print $cf "default_version = '1.0'\n";
	print $cf "relocatable = true\n";
	print $cf "requires = 'dump_test_ext_a,dump_test_ext_b'\n"
	  if $ext eq 'dump_test_ext_c';
	close $cf;

	# The extensions need no members, so an empty script will do.
	open my $sf, '>', "$tempdir/extension/$ext--1.0.sql"
	  or die "could not create script file for $ext: $!";
	close $sf;
}

my $sep = $windows_os ? ';' : ':';
my $ext_path = $windows_os ? ($tempdir =~ s/\\/\\\\/gr) : $tempdir;

# Create dump_test_ext_a before dump_test_ext_b, so that the requirement
# that sorts first by name is the one with the smaller OID.  pg_depend
# hands back these rows in descending OID order, that is, in the reverse of
# the order the dump must use.
$node->safe_psql(
	'postgres', qq{
	SET extension_control_path = '\$system$sep$ext_path';
	CREATE EXTENSION dump_test_ext_a;
	CREATE EXTENSION dump_test_ext_b;
	CREATE EXTENSION dump_test_ext_c;});

command_like(
	[ 'pg_dump', '--port' => $port, '--binary-upgrade', 'postgres' ],
	qr/\QSELECT pg_catalog.binary_upgrade_create_empty_extension('dump_test_ext_c', 'public', true, '1.0', NULL, NULL, ARRAY['dump_test_ext_a','dump_test_ext_b']::pg_catalog.text[]);\E/,
	'binary upgrade dumps required extensions in name order');

#########################################
# Verify that an object carrying labels from more than one security label
# provider gets its SECURITY LABEL commands emitted in provider name order,
# not in pg_seclabel/pg_shseclabel physical order.  dummy_seclabel registers
# a second provider, "dummy2", when dummy_seclabel.second_provider is turned
# on before the module is loaded.

SKIP:
{
	skip "dummy_seclabel module not installed", 6
	  unless $node->check_extension('dummy_seclabel');

	# Label each object with "dummy2" before "dummy", that is, in the reverse
	# of the order the dump has to use, so that emitting the labels in
	# catalog order would produce the wrong output.
	$node->safe_psql(
		'postgres', q|
		SET dummy_seclabel.second_provider = on;
		LOAD 'dummy_seclabel';
		CREATE TABLE seclabel_order_tbl (a int);
		SECURITY LABEL FOR dummy2 ON TABLE seclabel_order_tbl IS 'classified';
		SECURITY LABEL FOR dummy ON TABLE seclabel_order_tbl IS 'classified';
		SECURITY LABEL FOR dummy2 ON COLUMN seclabel_order_tbl.a IS 'classified';
		SECURITY LABEL FOR dummy ON COLUMN seclabel_order_tbl.a IS 'classified';
		SECURITY LABEL FOR dummy2 ON DATABASE postgres IS 'classified';
		SECURITY LABEL FOR dummy ON DATABASE postgres IS 'classified';
	|);

	$node->command_like(
		[ 'pg_dump', '--schema-only', 'postgres' ],
		qr/^
			\QSECURITY LABEL FOR dummy ON TABLE public.seclabel_order_tbl IS 'classified';\E\n
			\QSECURITY LABEL FOR dummy2 ON TABLE public.seclabel_order_tbl IS 'classified';\E\n
			\QSECURITY LABEL FOR dummy ON COLUMN public.seclabel_order_tbl.a IS 'classified';\E\n
			\QSECURITY LABEL FOR dummy2 ON COLUMN public.seclabel_order_tbl.a IS 'classified';\E$
			/xm,
		'security labels are dumped in provider order');

	$node->command_like(
		[ 'pg_dump', '--schema-only', '--create', 'postgres' ],
		qr/^
			\QSECURITY LABEL FOR dummy ON DATABASE postgres IS 'classified';\E\n
			\QSECURITY LABEL FOR dummy2 ON DATABASE postgres IS 'classified';\E$
			/xm,
		'shared security labels are dumped in provider order');
}

done_testing();
