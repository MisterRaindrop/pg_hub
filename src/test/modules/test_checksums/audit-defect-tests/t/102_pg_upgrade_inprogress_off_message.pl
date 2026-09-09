# Copyright (c) 2026, PostgreSQL Global Development Group

# DEFECT DEMONSTRATION for commit f19c0ec ("Online enabling and disabling of
# data checksums").
#
# pg_upgrade's control-data guard (src/bin/pg_upgrade/controldata.c) rejects an
# old cluster whose data-checksum state is any in-progress state:
#
#     if (oldctrl->data_checksum_version > PG_DATA_CHECKSUM_VERSION)
#         pg_fatal("data checksums are being enabled in the old cluster");
#
# Because the ChecksumStateType enum appends INPROGRESS_OFF (= 2) and
# INPROGRESS_ON (= 3) after PG_DATA_CHECKSUM_VERSION (= 1), the "> 1" test
# correctly fires for BOTH interrupted directions -- but the single hardcoded
# message only describes the "being enabled" direction.  For a cluster that was
# interrupted mid-DISABLE (state INPROGRESS_OFF, "Data page checksum version: 2"
# in the control file) the message states the exact OPPOSITE of what happened,
# and contradicts the feature's own canonical naming: SHOW data_checksums says
# 'inprogress-off' and pg_controldata prints version 2.
#
# This test builds an old cluster that comes cleanly to rest in INPROGRESS_OFF
# and inspects pg_upgrade's diagnostic.  Refusing the upgrade is CORRECT (the
# code comment says to "disallow the upgrade"), so the DEFECT assertions target
# only the wording, not the exit status.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Cwd qw(abs_path);

# ------------------------------------------------------------------ helpers

# Run a command; return (stdout, stderr, exitcode).
sub run_capture
{
	my (@cmd) = @_;
	my ($out, $err) = ('', '');
	IPC::Run::run(\@cmd, '>' => \$out, '2>' => \$err);
	my $rc = $? >> 8;
	return ($out, $err, $rc);
}

# Read checksum version and cluster state from an offline data dir via the
# pg_controldata binary.
sub controldata
{
	my ($bindir, $datadir) = @_;
	my ($out, $err, $rc) =
	  run_capture("$bindir/pg_controldata", '-D' => $datadir);
	die "pg_controldata failed (rc=$rc): $err" if $rc != 0;
	my ($ver) = $out =~ /^Data page checksum version:\s*(\d+)/m;
	my ($state) = $out =~ /^Database cluster state:\s*(.+?)\s*$/m;
	return ($ver, $state);
}

# ------------------------------------------------------------ set up old node

# initdb enables data checksums by default in this build, which is exactly the
# starting point for an online DISABLE.
my $old = PostgreSQL::Test::Cluster->new('old');
$old->init;
$old->append_conf('postgresql.conf',
	"shared_buffers = '16MB'\nmax_worker_processes = 16\n");
$old->start;

my $bindir = $old->config_data('--bindir');

is($old->safe_psql('postgres', 'SHOW data_checksums'),
	'on', 'old cluster starts with data checksums on (setup)');

# ---- Interrupt an online DISABLE so the cluster is pinned in INPROGRESS_OFF ----
#
# Hold one backend where it cannot absorb a procsignal barrier by SIGSTOPping
# it.  pg_disable_data_checksums() launches the "datachecksums launcher", which
# writes INPROGRESS_OFF to the control file and then blocks in
# WaitForProcSignalBarrier() waiting for the stopped backend -- so the state is
# deterministically pinned at INPROGRESS_OFF and cannot advance to full "off".

my $stall = $old->background_psql('postgres');
my $stall_pid = $stall->query('SELECT pg_backend_pid()');
$stall_pid =~ s/\D//g;
like($stall_pid, qr/^\d+$/, 'obtained a backend pid to stall (setup)');
is(kill('STOP', $stall_pid), 1, 'SIGSTOP the stall backend (setup)');

# The SQL function returns as soon as the launcher has been started.
$old->safe_psql('postgres', 'SELECT pg_disable_data_checksums()');

# Confirm the disable is pinned at inprogress-off.
ok( $old->poll_query_until(
		'postgres',
		"SELECT current_setting('data_checksums')",
		'inprogress-off'),
	'online disable is pinned at inprogress-off (setup)');

# Terminate the launcher directly.  launcher_exit() reverts only the
# INPROGRESS_ON direction, so an interrupted DISABLE keeps INPROGRESS_OFF.
my $launcher_pid = '';
foreach my $i (1 .. 100)
{
	$launcher_pid = $old->safe_psql('postgres',
		"SELECT pid FROM pg_stat_activity WHERE backend_type = 'datachecksums launcher'"
	);
	last if $launcher_pid ne '';
	usleep(100_000);
}
like($launcher_pid, qr/^\d+$/, 'located the datachecksums launcher (setup)');
is(kill('TERM', $launcher_pid), 1, 'SIGTERM the launcher (setup)');

# Wait until the launcher is gone, so nothing can complete the transition.
ok( $old->poll_query_until(
		'postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'datachecksums launcher'",
		'0'),
	'launcher has exited, state left at inprogress-off (setup)');

# With no launcher, it is now safe to resume the stalled backend.
is(kill('CONT', $stall_pid), 1, 'SIGCONT the stall backend (setup)');
$stall->quit;

is($old->safe_psql('postgres', 'SHOW data_checksums'),
	'inprogress-off',
	'state remains inprogress-off after launcher exit (setup)');

# A clean (fast) shutdown persists the interrupted state in the control file.
$old->stop('fast');

my ($old_ver, $old_state) = controldata($bindir, $old->data_dir);
is($old_ver, '2',
	'old control file records checksum version 2 (inprogress-off) (setup)');
like($old_state, qr/shut down/, 'old cluster is cleanly shut down (setup)');

# ------------------------------------------------------------ set up new node

my $new = PostgreSQL::Test::Cluster->new('new');
$new->init;
$new->append_conf('postgresql.conf',
	"shared_buffers = '16MB'\nmax_worker_processes = 16\n");
# The new cluster need not be started: pg_upgrade reads control data (and fires
# this diagnostic) before starting any server.

# ------------------------------------------------------------ run pg_upgrade

# Resolve absolute paths (node data_dir/host are relative to the harness cwd)
# and leave cwd unchanged (it is a writable temp dir where pg_upgrade may create
# its output directory).
my $old_datadir = abs_path($old->data_dir);
my $new_datadir = abs_path($new->data_dir);
my $sockdir = abs_path($new->host);

my @cmd = (
	"$bindir/pg_upgrade", '--check', '--no-sync',
	'--old-datadir' => $old_datadir,
	'--new-datadir' => $new_datadir,
	'--old-bindir' => $bindir,
	'--new-bindir' => $bindir,
	'--socketdir' => $sockdir,
	'--username' => 'postgres');

my ($out, $err, $rc) = run_capture(@cmd);
my $msg = "$out\n$err";
note "pg_upgrade --check exit=$rc; output:\n$msg";

# Refusing to upgrade a cluster mid-transition is the correct behavior.
is($rc, 1,
	'pg_upgrade --check refuses an in-progress old cluster (correct behavior)');

# DEFECT: the old cluster was interrupted while DISABLING checksums
# (inprogress-off / control-file version 2).  A correct diagnostic must NOT tell
# the user checksums are "being enabled"; it should name the disable direction
# or be direction-neutral.
unlike($msg, qr/being enabled/i,
	'pg_upgrade must not report "being enabled" for a cluster interrupted mid-DISABLE');

# DEFECT: the diagnostic should reflect the actual situation -- a disable in
# progress (inprogress-off) -- consistent with SHOW data_checksums and
# pg_controldata (version 2).
like($msg, qr/disabl|in ?progress|processing/i,
	'pg_upgrade diagnostic should reflect the disable/in-progress direction');

done_testing();
