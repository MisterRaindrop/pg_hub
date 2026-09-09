# Audit of commit f19c0ec — user-visible defects in online data checksums

This branch reports **six user-visible defects** introduced by PostgreSQL commit
`f19c0ec` ("Online enabling and disabling of data checksums") that are **still
present** at the tip of branch `f19c0ec-checksum-online` (commit `50bb165`), which
already carries four follow-up bug-fix commits. Each defect has a standalone
reproducer TAP test that fails on this branch precisely because the defect is
present. One further real problem was found but is **out of scope** (a follow-up
commit, not `f19c0ec`, introduced it); four other candidates were **refuted**.

Everything on this branch — this report, the tests, and `PROVENANCE.md` — was
produced by an automated, model-driven audit and then verified by the repository
owner's own review and re-runs. See `PROVENANCE.md` for exactly how, including
the model, the prompt, and the parts the automation got wrong before correction.

## Scope and method

* **Audited commit:** `f19c0ec` (Daniel Gustafsson, 2026-04-03).
* **Present-at-tip means:** observable at `50bb165`, i.e. after the four
  post-`f19c0ec` follow-ups on this branch: `05e7b3e` (replay no longer adopts
  another node's checksum state), `1d4d52d` (pg_checksums refuses interrupted
  transitions), `85f7eed` (pg_rewind checks source/target states), `50bb165`
  (pg_combinebackup refuses a mixed-state chain).
* **A defect** is behavior contrary to the feature's own binding principles —
  its documentation, the invariants stated in its commit message and code
  comments, plain correctness, and consistency with how the rest of PostgreSQL
  behaves in the equivalent situation — that a user can observe (through SQL, a
  client tool, a log line, wrong monitoring output, a crash, a hang, or
  corruption). Defects already fixed by the four follow-ups, or already asserted
  by the shipped tests `t/001`–`t/024`, are out of scope.
* **How they were verified:** each defect was reproduced on a live build
  installed from this branch (meson, `cassert`+`debug`+`tap_tests`+
  `injection_points`), and each is attributed to `f19c0ec` by `git blame` and by
  confirming the four follow-ups do not touch the cited code. The reproducer
  tests were re-run by hand against this branch; all fail at their `# DEFECT:`
  assertions while their setup/context assertions pass.

## Severity at a glance

| # | Defect | Severity | Test |
|---|--------|----------|------|
| D1 | Cancelling the enable launcher lets a second launcher start; the race logs a false "data checksums are now enabled" while the cluster is off, and the enable is silently lost | medium | `t/101` |
| D2 | Killing the launcher mid-*disable* leaves the cluster in `inprogress-off` with nothing to finish the transition (an interrupted *enable* self-heals; a disable does not) | low–medium | `t/105` |
| D3 | `pg_upgrade` tells the user checksums are "being enabled" when the old cluster was interrupted mid-*disable* | low | `t/102` |
| D4 | `pg_settings.enumvals` for `data_checksums` is the phantom `{""}` instead of the value list or NULL | low | `t/103` |
| D5 | `pg_stat_progress_data_checksums.blocks_total`/`blocks_done` are per-fork though documented as per-relation, so `blocks_done` moves backward within one relation | low | `t/104` |
| D6 | `pg_enable/disable_data_checksums` ship the delegable `{POSTGRES=X}` ACL yet a hardcoded `superuser()` gate makes a granted `EXECUTE` silently ineffective; the docs state no privilege model | low | `t/106` |

All `file:line` references below are at this branch tip (`50bb165`).

## Confirmed defects

### D1 — a cancelled enable launcher lets a second launcher start, logging a false "now enabled" while checksums are off

**User-visible effect.** After a superuser cancels an in-progress
`pg_enable_data_checksums()` (for example with `pg_cancel_backend()`) and a
concurrent `pg_enable_data_checksums()` runs, the server log records
`data checksums are now enabled` even though `SHOW data_checksums` and
`pg_controldata` both report **off**. The operator is told, in the log, that
their data is now checksum-protected when it is not, and the enable they issued
is silently lost.

**Mechanism.** Cancelling the launcher makes `DataChecksumsWorkerLauncherMain`
fall through to its `done:` label, which clears
`DataChecksumState->launcher_running` at `datachecksum_state.c:1398-1399` while
the cluster state is still `inprogress-on`. The mandatory reset to off is
deferred to the `launcher_exit` shmem-exit callback
(`datachecksum_state.c:1155-1156`), which runs the slow `SetDataChecksumsOff()`
(two procsignal barriers plus two forced `CHECKPOINT_WAIT` checkpoints).
Because `StartDataChecksumsWorkerLauncher` gates a new launcher solely on
`launcher_running` (`:664`, `:682`), a concurrent `pg_enable_data_checksums()`
in that window starts a **second** launcher. The two race; the surviving one
reaches `SetDataChecksumsOn()` (`xlog.c:4852`) with the state no longer
`inprogress-on`, so it emits `WARNING: cannot set data checksums to "on",
current state is not "inprogress-on", disabling` and disables — yet
`LauncherMain` then logs `data checksums are now enabled` unconditionally
(`datachecksum_state.c:1351-1354`). This breaks the single-launcher invariant
the feature was built around.

**Test.** `t/101_launcher_cancel_double_launcher.pl` drives the window
deterministically with the in-tree `injection_points` module (freezing the
checkpointer at `create-checkpoint-initial`). The load-bearing assertion — the
launcher must not log "data checksums are now enabled" while `data_checksums` is
off — fails on this branch. Corroborating assertions (a second launcher with a
new PID appears; the state moves backward `inprogress-off` → `inprogress-on`)
pass only because the defect is present.

### D2 — an interrupted *disable* is not self-healed, unlike an interrupted *enable*

**User-visible effect.** If the datachecksums launcher is terminated
(`pg_terminate_backend()`) while a `pg_disable_data_checksums()` is in its
`inprogress-off` phase, the cluster is left in `inprogress-off` with no process
to complete the transition to off. `SHOW data_checksums` reports
`inprogress-off` indefinitely. The state is recoverable — re-issuing
`pg_disable_data_checksums()` or restarting the server both finish it — but it
does not resolve on its own, in contrast to an interrupted *enable*, which the
same code path heals back to off.

**Mechanism.** `launcher_exit` (`datachecksum_state.c:1155-1156`) contains a
branch for an interrupted enable only:

```c
if (DataChecksumsInProgressOn())
    SetDataChecksumsOff();
```

There is no symmetric branch for an interrupted disable (no
`DataChecksumsInProgressOff()` helper exists), and `bgw_restart_time` is
`BGW_NEVER_RESTART`, so nothing advances a disable that was interrupted after
`SetDataChecksumsOff()` had persisted `inprogress-off` but before it reached
off. This is an asymmetry in the feature's own cleanup logic, not a data-safety
problem: in `inprogress-off` checksums are written but not verified, so no page
fails verification.

**Test.** `t/105_launcher_kill_disable_wedge.pl` contrasts the two directions
with the same kill. The control (kill mid-enable → self-heals to off) passes;
the defect assertion (kill mid-disable → should also reach off) fails, and the
trailing assertions confirm the state is recovered by a re-issued
`pg_disable_data_checksums()`.

### D3 — `pg_upgrade` reports the wrong direction for a cluster interrupted mid-disable

**User-visible effect.** `pg_upgrade` (and `pg_upgrade --check`) on an old
cluster left in `inprogress-off` fails with `checksums are being enabled in the
old cluster` — the exact opposite of what happened (the cluster was being
*dis
says `inprogress-off`; `pg_controldata` prints version 2). Refusing the upgrade
is correct; only the wording is wrong.

**Mechanism.** `src/bin/pg_upgrade/controldata.c:661-662`:

```c
if (oldctrl->data_checksum_version > PG_DATA_CHECKSUM_VERSION)
    pg_fatal("data checksums are being enabled in the old cluster");
```

The `ChecksumStateType` enum (`storage/checksum.h`) appends `INPROGRESS_OFF = 2`
and `INPROGRESS_ON = 3` after `PG_DATA_CHECKSUM_VERSION = 1`, so the `> 1` test
correctly fires for **both** interrupted directions, but the single hardcoded
message describes only the enable direction. (Commit `078aac2e` later prepended
"data " to the string; it did not fix the direction, and is not one of the four
scoped follow-ups.)

**Test.** `t/102_pg_upgrade_inprogress_off_message.pl` brings a cluster cleanly
to rest in `inprogress-off` (SIGSTOP a backend so the disable launcher blocks on
its procsignal barrier, then SIGTERM the launcher) and asserts the `pg_upgrade`
diagnostic does not claim the enable direction. It fails on this branch.

### D4 — `pg_settings.enumvals` for `data_checksums` is the phantom `{""}`

**User-visible effect.** `SELECT enumvals FROM pg_settings WHERE name =
'data_checksums'` returns a one-element array whose single element is the empty
string (`{""}`), in every state. A correct enum GUC reports either NULL (as the
pre-`f19c0ec` boolean `data_checksums` did) or its list of valid values (as
`wal_level` and the sibling read-only preset enum `huge_pages_status` do). The
universal invariant `enumvals IS NULL OR setting = ANY(enumvals)`, which every
other enum GUC satisfies, is violated: the current setting is never a member of
its own advertised value list.

**Mechanism.** `f19c0ec` turned `data_checksums` from bool into an enum GUC but
marks all four entries of `data_checksums_options[]` hidden
(`guc_tables.c:515-521`, third field `true`). With every entry hidden,
`config_enum_get_options()` appends no names, so `pg_settings` builds the bogus
`{""}` instead of NULL or the value list.

**Test.** `t/103_data_checksums_enumvals.pl` checks the invariant against sibling
enum GUCs (which pass) and against `data_checksums` (which fails), in both the
off and on states.

### D5 — progress view block counters are per-fork though documented as per-relation

**User-visible effect.** While one relation is being processed,
`pg_stat_progress_data_checksums.blocks_total` changes several times and
`blocks_done` moves **backward**. `monitoring.sgml` documents both as "the
number of blocks in the current relation" (total / processed), which implies a
per-relation quantity that only grows. A monitoring tool or DBA watching the
view sees `blocks_done` reset and `blocks_total` jump (for example to the main,
then fsm, then vm fork sizes of one table).

**Mechanism.** The worker sets `BLOCKS_TOTAL`/`BLOCKS_DONE` per fork in
`ProcessSingleRelationFork` (`datachecksum_state.c:708-731`, and `:749`/`:785`
update `BLOCKS_DONE`), so the counters describe the current *fork*, not the
current *relation* as documented.

**Test.** `t/104_progress_blocks_per_fork.pl` takes per-fork ground truth from
`pg_relation_size('big','main'|'fsm'|'vm')`, throttles the enable with a per-block
cost delay so the small forks stay observable, and asserts (a) `blocks_total`
equals the whole-relation block count while the relation is current, and (b)
`blocks_done` never decreases while the relation is current. Both fail on this
branch.

### D6 — a granted `EXECUTE` on the checksum functions is silently defeated by an internal `superuser()` gate

**User-visible effect.** A superuser runs `GRANT EXECUTE ON FUNCTION
pg_enable_data_checksums(int,int) TO alice`; the grant succeeds and
`has_function_privilege('alice', 'pg_enable_data_checksums(int,int)', 'execute')`
returns true. Yet `alice` (a non-superuser) calling the function gets `ERROR:
must be superuser to change data checksum state`. For the identically-ACL'd
`pg_reload_conf()` and `pg_switch_wal()`, the same grant lets `alice` run them.
The recorded privilege is therefore a lie for these functions alone, and the
documentation (`func-admin.sgml`, "Data Checksum Functions") states no privilege
model at all, unlike every neighboring restricted admin function.

**Mechanism.** Both functions ship the delegable ACL `proacl => '{POSTGRES=X}'`
in `pg_proc.dat` — the ACL that, for the ~70 other admin functions carrying it,
means "may be delegated with GRANT." But `f19c0ec` also hardcodes a
`superuser()` gate in the bodies (`datachecksum_state.c:574-577` for disable,
`:596-599` for enable), so a grant that the executor accepts is then rejected
inside the function. Either the gate is redundant (drop it and honor the grant)
or the ACL should not be delegable; shipping both is contradictory, and unique
among `{POSTGRES=X}` functions.

**Test.** `t/106_checksum_func_grant_defeated.pl` grants both functions to a
non-superuser, confirms `has_function_privilege` is true (as for the passing
control `pg_reload_conf`), and asserts — fix-agnostically — that a recorded
`EXECUTE` grant actually permits execution. It fails on this branch because the
grantee is rejected with "must be superuser". (A fix that instead makes the ACL
non-delegable would flip `has_function_privilege` to false and satisfy the
assertion vacuously.)

## Found, but out of scope

**`wal.sgml` "Replication" impact bullet is stale at the tip.** The bullet
(`doc/src/sgml/wal.sgml`, ~lines 433-443) tells DBAs that a standby receiving an
online checksum-state change "will issue a restartpoint" to flush the state into
`pg_control`, that "the restartpoint will block redo," and that reducing
`max_wal_size` shortens it. At the tip the state is persisted **inline** during
redo via `UpdateControlFile()` and the only redo stall is a procsignal-barrier
wait (`EmitAndWaitDataChecksumsBarrier`), which `max_wal_size` does not affect;
no restartpoint is issued by the transition. So the documented cause and
mitigation are wrong. **This is excluded** because the paragraph was accurate for
`f19c0ec` as written; the behavior it describes was changed by follow-up
`05e7b3e` (which reworked standby replay to persist inline), which left the
paragraph stale. The defect was therefore introduced by a follow-up, not by
`f19c0ec`. It is recorded here for completeness and is worth fixing on its own.

## Considered and refuted

* **"`LauncherMain` logs 'now enabled' unconditionally."** True structurally, but
  in ordinary single-launcher operation `SetDataChecksumsOn()` succeeds, so the
  log is truthful. The false log is reachable only via the D1 race, where it is
  reported.
* **"`pg_controldata` diverges from `pg_control_init()`."** The claim that the
  SQL/CLI mirror is broken did not hold up: both expose the checksum version
  consistently.
* **"Transaction-wait is not surfaced in the progress view."** The `enabling`
  phase is the documented phase during the initial transaction wait; no separate
  phase is promised for it.
* **"Enable waits only for transactions holding an XID, not all open
  transactions."** Waiting on XID-holding transactions is the correct and
  sufficient condition; a read-only open transaction cannot have created the
  unlogged/new relations the wait exists to serialize against.

## Running the tests

The tests are standalone `PostgreSQL::Test` TAP scripts kept **outside** the
shipped `meson` suite (they are reproducers that fail on this un-fixed branch, so
wiring them into `meson test` would break CI by design). Run them against a build
of this branch:

```sh
# build this branch with TAP + injection points, install to $INST
meson setup build . -Dtap_tests=enabled -Dinjection_points=true \
    -Dcassert=true -Ddebug=true --prefix=$INST
ninja -C build install

# run a reproducer (101, 105, 106 use injection points)
PATH=$INST/bin:$PATH \
PG_REGRESS=$PWD/build/src/test/regress/pg_regress \
PERL5LIB=$PWD/src/test/perl \
enable_injection_points=yes \
    prove -v src/test/modules/test_checksums/audit-defect-tests/t/101_launcher_cancel_double_launcher.pl
```

Each test prints its `# DEFECT:` assertions as `not ok` on this branch, with
diagnostics naming the observed wrong value; setup/context assertions pass. A
correct fix for the corresponding defect turns the `not ok` into `ok`.

## Caveats

* The tests demonstrate the defects; they are **not** a fix, and this branch
  contains no source change. Each test is written so it would pass under a
  correct fix and fail now.
* D1, D2, and D3 depend on `injection_points` (D1/D2) or on reaching a genuine
  `inprogress-off` state (D3); they are deterministic given those tools but do
  drive process-timing windows. D4, D5, and D6 are straightforwardly
  deterministic.
* Severities are the auditor's judgement. None of the six risks data corruption
  or a false checksum failure; D1 (a misleading "now enabled" log plus a silently
  lost enable) is the most consequential operationally.
