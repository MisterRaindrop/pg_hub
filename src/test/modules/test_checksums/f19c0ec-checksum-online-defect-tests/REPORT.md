# User-visible defects introduced by `f19c0ec`

## Scope and result

This audit examined commit `f19c0ec` ("Online enabling and disabling of data
checksums") and retested candidate defects at commit
`50bb165a05199c2cf93c1c9f42ddbca4162bee7a`, the tip of
`f19c0ec-checksum-online` during the audit.  It found seven user-visible
defects introduced by `f19c0ec` that remain at that tip.  Two have standalone
TAP regression tests in this directory; five are documented without a
committed test because a functional test would be platform-specific,
fix-hostile, or unable to distinguish the defective documentation from its
fix.

| Defect | User impact | Coverage here |
|---|---|---|
| A late temporary table is treated as pre-existing | Online enablement waits for a session that it need not wait for | `t/002_late_temp_table.pl` |
| `pg_settings.enumvals` is `{""}` | Monitoring and introspection clients receive false metadata | `t/001_enumvals.pl` |
| A registered worker can outlive a terminated launcher | Relation I/O and full-page-image WAL can begin after `SHOW data_checksums` reports `off` and the launcher has exited | Live-reproduced; report only |
| `pg_upgrade` misreports `inprogress-off` | Its fatal diagnostic says checksums are being enabled while they are being disabled | Report only |
| Progress block counters are per fork, not per relation as documented | Monitors see unexplained resets and totals | Report only |
| The documented function signature omits a valid one-argument call | Users are not told they can set only `cost_delay` | Report only |
| The function documentation omits the mandatory superuser requirement | Users cannot tell who is authorized to run the operations | Report only |

## Tested defects

### 1. Late-created temporary tables can delay enablement

The monitoring documentation says that the worker waits for temporary tables
that existed "at the time the command was started"
(`doc/src/sgml/monitoring.sgml:8386-8390`).  That cutoff is important: once the
cluster has entered `inprogress-on`, pages belonging to newly created
temporary tables receive checksums when written and do not require rewriting.

The implementation instead builds `InitialTempTableList` separately when each
per-database worker starts
(`src/backend/postmaster/datachecksum_state.c:1793-1800`) and later waits for
the OIDs in that list (`:1918-1954`).  Since the launcher processes databases
serially, a temporary table can be created after
`pg_enable_data_checksums()` has started but before the worker for that
database is launched.  The worker misclassifies that table, whose pages will
receive checksums when written, as pre-existing and waits until its owning
session exits.  A long-lived session can therefore make the cluster remain in
`inprogress-on` indefinitely, contrary to the documented cutoff.

`t/002_late_temp_table.pl` creates the table strictly after the command has
entered `inprogress-on`, but before its database worker can start.  On the
audited tip, setup assertions 1-4 pass and the final assertion fails with:

```text
online checksum enable outcome with late temp table: blocked-by-late-temp
```

The test then closes the late session and waits for enablement to finish before
reporting the failure, so it leaves no worker or server behind.

### 2. `pg_settings` advertises no usable enum values

`f19c0ec` changed `data_checksums` from a boolean GUC to an enum, but marked
all four enum entries hidden
(`src/backend/utils/misc/guc_tables.c:515-520`).  Consequently the
`pg_settings` row has `vartype = 'enum'` and a current setting such as `off`,
but `enumvals = '{""}'`.  This contradicts the documented meaning of
`enumvals` as the allowed values of an enum parameter
(`doc/src/sgml/system-views.sgml:3826-3833`).  The generic GUC code itself
calls an all-hidden enum a "broken GUC setup"
(`src/backend/utils/misc/guc.c:2991-2996`).

Monitoring tools and other clients that inspect type metadata through
`pg_settings` cannot validate or present this setting
correctly.  If `data_checksums` remains an enum, `t/001_enumvals.pl` requires
the four states introduced by `f19c0ec` to be present and the current setting
to be one of the advertised values.  It also permits a fix that represents
this read-only status as a non-enum setting with `enumvals` set to `NULL`, as
long as the displayed status remains one of the four defined states.  On the
audited tip its only assertion fails, with the diagnostic:

```text
type=enum setting=off enumvals={""}
```

The containment assertion permits future states and does not prescribe the
ordering of the four existing states.

## Confirmed defects not covered by a committed test

### 3. A per-database worker can start after launcher termination

The launcher publishes the intended database and invocation, registers a
dynamic background worker, and waits for it to start
(`src/backend/postmaster/datachecksum_state.c:974-1020`).  It does not publish
the worker PID until after startup succeeds (`:1071-1080`).  If the launcher
receives `SIGTERM` in that interval, its exit callback sees an invalid
`worker_pid`, cannot signal the registered worker (`:1133-1148`), and resets
the cluster from `inprogress-on` to `off` (`:1151-1156`).  Background-worker
notification cleanup clears only the dead launcher's notification PID; it
does not cancel the registration (`src/backend/postmaster/bgworker.c:413-427,
540-551`).  The stored invocation remains current, so the delayed worker's
checks do not reject it (`datachecksum_state.c:1776-1806`).
The register-wait-publish ordering and missing registration cancellation were
already present in `f19c0ec`; later invocation and PID bookkeeping changes did
not close the window.

This was reproduced live.  The postmaster process was paused with `SIGSTOP`
while `pg_stat_activity` showed the launcher waiting in `BgWorkerStartup`;
`pg_terminate_backend()` then terminated the launcher and `SHOW
data_checksums` reported `off` with no launcher.  After the postmaster was
resumed with `SIGCONT`, the registered worker started.  While checksums were
still `off` and no launcher existed, `pg_stat_activity` showed:

```text
datachecksums worker | active | IPC | ChecksumEnableTemptableWait
```

and `pg_stat_progress_data_checksums` contained its progress row.  The server
log first reported that the launcher notification PID was invalid and then
started the worker.  The orphan can read and dirty pages and emit
full-page-image WAL records for its whole database after `SHOW data_checksums`
reports `off` and the launcher has exited.  It no longer calculates checksums
once the state is `off`.  The remaining relation I/O, WAL, and replication
traffic are unexpected, though the audit found no corruption path.

A deterministic reproducer requires Unix process control to pause the
postmaster with `SIGSTOP` in a narrow background-worker startup phase, with
every monitoring connection established beforehand.  An upstream test could
instead add a purpose-built injection point.  The current live reproducer is
therefore not included as a portable regression test here.

### 4. `pg_upgrade` reports the wrong in-progress direction

The control-file enum assigns distinct values to
`PG_DATA_CHECKSUM_INPROGRESS_OFF` and `PG_DATA_CHECKSUM_INPROGRESS_ON`
(`src/include/storage/checksum.h:26-32`).  `pg_upgrade` correctly refuses both,
but classifies every value greater than the enabled version as "data checksums
are being enabled" (`src/bin/pg_upgrade/controldata.c:656-662`).  The
classification and original "checksums are being enabled" diagnostic came
from `f19c0ec`; later commit `078aac2` only inserted the word "data".  A
cluster whose state is `inprogress-off` therefore receives a factually
reversed fatal diagnostic.  That can send an administrator toward the wrong
recovery action.

Producing a naturally generated, cleanly shut-down `inprogress-off` old
cluster on demand requires holding the disable barrier and externally
interrupting its launcher.  That would couple the diagnostic test to
contentious signal-state semantics rather than isolate `pg_upgrade`.  Directly
manufacturing the internal control-file value would be brittle, so no test is
included.

### 5. Block progress counters have a different unit than documented

The progress view documents `blocks_total` and `blocks_done` as blocks in the
current *relation* (`doc/src/sgml/monitoring.sgml:8317-8339`).  In fact,
`ProcessSingleRelationByOid()` iterates relation forks and
`ProcessSingleRelationFork()` resets both counters for each fork
(`src/backend/postmaster/datachecksum_state.c:725-750, 839-879`).  For a
relation with main, FSM, and VM forks, the counters reset or change totals
while `relations_done` remains unchanged.  The view has no fork column, so a
monitor cannot interpret those transitions from the documented contract.
The per-fork unit dates to `f19c0ec`; later commit `e981025` made the
`blocks_done` reset explicit without changing that unit.

Both an implementation fix that aggregates forks and a documentation fix that
defines the counters as per-fork would resolve the mismatch.  A runtime test
demanding monotonic relation-wide values would reject the valid
documentation-only resolution and would also depend on observing a brief
phase.  This defect is report-only pending a choice of contract.

### 6. The documented signature hides a supported call form

The catalog gives defaults to both trailing arguments of
`pg_enable_data_checksums(int, int)`
(`src/include/catalog/pg_proc.dat:12478-12483`), so zero-, one-, and
two-argument calls are valid.  The documentation encloses both parameters in
one optional group and discusses only specifying both
(`doc/src/sgml/func/func-admin.sgml:3155-3176`).  It therefore does not show
the valid `pg_enable_data_checksums(cost_delay)` form, which changes the delay
while retaining the default cost limit.

A positive SQL test of the one-argument call already passes on the defective
branch and after a documentation fix.  A source-text assertion against SGML
markup would be brittle, so this documentation defect has no functional test.

### 7. The documentation omits the superuser requirement

The same function section describes both checksum state-changing functions
without an authorization requirement
(`doc/src/sgml/func/func-admin.sgml:3126-3202`).  Both implementations
unconditionally reject callers for whom `superuser()` is false
(`src/backend/postmaster/datachecksum_state.c:574-577, 596-599`).  A reader of
the manual therefore cannot determine who may perform the documented
operation.  If the hard check is intentionally nondelegable, the manual
should also make clear that granting `EXECUTE` does not suffice.

A runtime denial test would exercise the existing intended implementation and
would continue to pass after the missing documentation is supplied.  A test
that greps prose would be inappropriate, so this is report-only.

## Leads evaluated but not counted

- Cancel-and-immediately-reenable behavior at the branch tip can launch
  overlapping work and produce a false success log.  In `f19c0ec`, however,
  `SIGINT` cancellation eventually raised `ERROR`; the exit callback repaired
  the checksum state while `launcher_running` remained true, so an immediate
  same-direction request could not start a second launcher.  Later commits
  `8fb8ded`, `bf25e55`, `a4f02ca`, and `ed6775d` supplied the state
  transitions and early `done:` path that make the overlap reachable.  The
  clear-before-callback ordering was latent in `f19c0ec`, but the user-visible
  defect was introduced by those follow-ups and is outside this report's
  stated scope.
- Killing a disable launcher can leave or briefly expose `inprogress-off`, but
  the audit found no documented post-`SIGTERM` state contract and the operation
  is recoverable by reissuing the command or restarting.  A test that insists
  on one cleanup policy would encode an unestablished requirement.
- A non-superuser remains rejected after `GRANT EXECUTE` on the checksum
  functions.  That does not by itself prove a code defect: function ACLs and
  authorization checks inside a function can legitimately coexist.  Only the
  objective documentation omission is counted above.
- The replication-impact text still says a standby issues a restartpoint that
  blocks redo and recommends reducing `max_wal_size`.  Commit `05e7b3e` changed
  replay to persist the state inline without that restartpoint, leaving the
  text stale; the defect therefore postdates `f19c0ec`.
- `50bb165` treats a final incremental backup in `inprogress-off` as enabled
  when checking a chain whose prior full backup was `on`, and can falsely
  report that only some backups have checksums enabled.  That issue was
  introduced by the target-tip commit itself and is not covered by its
  `t/024_combinebackup_mixed.pl` test.
- Missing visual padding in one `pg_controldata` label and progress values
  retained while a worker waits on temporary tables do not violate a stated
  interface and were not classified as defects.

## Running the tests

These tests are intentionally standalone and are not connected to the regular
`test_checksums` Meson target: both are regression reproducers expected to fail
at `50bb165`.  Run them from an empty scratch directory with the usual
PostgreSQL TAP environment (`PATH` pointing at the temporary installation,
`PERL5LIB` at `src/test/perl`, and `PG_REGRESS`, `REGRESS_SHLIB`, and
`INITDB_TEMPLATE` set as for the in-tree TAP suite):

```sh
prove -v /path/to/f19c0ec-checksum-online-defect-tests/t/001_enumvals.pl
prove -v /path/to/f19c0ec-checksum-online-defect-tests/t/002_late_temp_table.pl
```

The late-temp script stops its cluster before its intended failing assertion;
the enum-metadata script stops its cluster immediately afterward.
