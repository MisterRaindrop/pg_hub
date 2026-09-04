# 106_checksum_func_grant_defeated.pl
#
# Reproducer for a privilege-model defect introduced by commit f19c0ec
# ("Online enabling and disabling of data checksums").
#
# THE DEFECT
#   pg_enable_data_checksums() and pg_disable_data_checksums() ship the
#   *delegable* catalog ACL proacl => '{POSTGRES=X}' (pg_proc.dat) -- the exact
#   ACL carried by pg_switch_wal(), pg_reload_conf(), pg_backup_start() and the
#   ~70 other admin functions whose access is, by PostgreSQL convention,
#   "managed through the normal GRANT system": a superuser may GRANT EXECUTE to
#   a non-superuser to delegate them.  But f19c0ec ALSO hardcodes a superuser()
#   gate in the function bodies (datachecksum_state.c enable_data_checksums /
#   disable_data_checksums: "must be superuser to change data checksum state").
#
#   So a GRANT EXECUTE to a non-superuser is accepted and recorded in proacl
#   (has_function_privilege() returns true), yet the grantee is still rejected
#   at run time -- these are the only {POSTGRES=X} functions whose recorded
#   EXECUTE privilege does not actually permit execution.  The docs
#   (func-admin.sgml "Data Checksum Functions") compound it by stating no
#   privilege model at all, unlike every neighboring restricted function.
#
#   The assertion below is deliberately FIX-AGNOSTIC: it only requires that the
#   catalog's recorded privilege AGREE with real executability.  A fix that
#   drops the redundant superuser() gate (has_priv stays true, execution works)
#   AND a fix that makes the ACL non-delegable (has_priv becomes false) both
#   satisfy it; only the current inconsistent state fails.  pg_reload_conf(),
#   with the identical {POSTGRES=X} ACL, is the passing control.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('csum_grant');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', "shared_buffers = 16MB\nmax_worker_processes = 16\n");
$node->start;

$node->safe_psql('postgres', 'CREATE ROLE alice LOGIN NOSUPERUSER');

# has_function_privilege for alice on a function signature.
sub has_priv
{
	my ($sig) = @_;
	return $node->safe_psql('postgres',
		"SELECT has_function_privilege('alice', '$sig', 'execute')");
}

# Does alice actually get to run $call?  Returns 'ok' on success, else the
# SQLSTATE-bearing error message.  Uses psql as alice via SET ROLE.
sub alice_can_run
{
	my ($call) = @_;
	my ($stdout, $stderr) = ('', '');
	$node->psql('postgres', "SET ROLE alice; $call",
		stdout => \$stdout, stderr => \$stderr, on_error_stop => 1);
	# Any server error (permission denied, must be superuser, ...) lands on
	# stderr; empty stderr means the call actually ran.
	return ($stderr =~ /\S/) ? $stderr : 'ok';
}

# ---- setup / context (PASS) --------------------------------------------
# Before any grant, the ACL layer denies alice (permission denied) -- proves
# PUBLIC has no execute and alice is a plain non-superuser.
like(alice_can_run('SELECT pg_enable_data_checksums()'),
	qr/permission denied for function pg_enable_data_checksums/,
	'setup: without a grant, alice is denied by the ACL layer');

# The identically-ACL'd control function: grant it, and the recorded privilege
# both reads true AND actually lets alice run it.
$node->safe_psql('postgres', 'GRANT EXECUTE ON FUNCTION pg_reload_conf() TO alice');
is(has_priv('pg_reload_conf()'), 't',
	'control: GRANT recorded for pg_reload_conf (has_function_privilege = t)');
is(alice_can_run('SELECT pg_reload_conf()'), 'ok',
	'control: a {POSTGRES=X} function honors the GRANT -- alice can run pg_reload_conf');

# ---- grant the checksum functions to alice ------------------------------
$node->safe_psql('postgres',
	'GRANT EXECUTE ON FUNCTION pg_enable_data_checksums(int,int) TO alice');
$node->safe_psql('postgres',
	'GRANT EXECUTE ON FUNCTION pg_disable_data_checksums() TO alice');

# The GRANT is accepted and recorded, exactly as for the control (PASS).
is(has_priv('pg_enable_data_checksums(int,int)'), 't',
	'GRANT EXECUTE on pg_enable_data_checksums is recorded (has_function_privilege = t)');
is(has_priv('pg_disable_data_checksums()'), 't',
	'GRANT EXECUTE on pg_disable_data_checksums is recorded (has_function_privilege = t)');

my $enable_err = alice_can_run('SELECT pg_enable_data_checksums()');
my $disable_err = alice_can_run('SELECT pg_disable_data_checksums()');
diag("alice pg_enable_data_checksums() -> $enable_err");
diag("alice pg_disable_data_checksums() -> $disable_err");

# ---- the defect (FAIL on this branch) -----------------------------------

# DEFECT: the catalog's recorded EXECUTE privilege must agree with real
# executability, as it does for pg_reload_conf and every other {POSTGRES=X}
# function.  Since has_function_privilege('alice', ...) is true (asserted
# above), alice must be able to execute the function -- i.e. NOT be rejected
# with "must be superuser".  On this branch the internal superuser() gate
# rejects her, so the recorded privilege is a lie.  (Fix-agnostic: a fix that
# instead makes the ACL non-delegable would flip has_function_privilege to
# false and this test's has_priv assertions, not this one, would change.)
unlike($enable_err, qr/must be superuser to change data checksum state/,
	'DEFECT: a recorded EXECUTE grant on pg_enable_data_checksums must actually permit execution');
unlike($disable_err, qr/must be superuser to change data checksum state/,
	'DEFECT: a recorded EXECUTE grant on pg_disable_data_checksums must actually permit execution');

$node->stop;
done_testing();
