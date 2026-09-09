# audit-defect-tests — reproducers for f19c0ec defects still present at this tip

These six standalone TAP tests each **demonstrate a distinct user-visible defect**
introduced by commit `f19c0ec` ("Online enabling and disabling of data checksums")
and still present at the tip of branch `f19c0ec-checksum-online`. The full write-up
(mechanism, `file:line`, ancestry, severity) is in `AUDIT-REPORT-checksum-online.md`
at the repository root; provenance is in `PROVENANCE.md`.

They are deliberately kept **out of the `meson`/`make` test suite**: this branch
carries no fix, so every test fails on purpose (that failure is the demonstration),
and wiring them in would break CI. A correct fix for a given defect turns its
`# DEFECT:` assertion from `not ok` into `ok`.

| Test | Defect | Uses injection points |
|------|--------|:---:|
| `t/101_launcher_cancel_double_launcher.pl` | D1: cancelled enable launcher → second launcher → false "now enabled" log while off | yes |
| `t/102_pg_upgrade_inprogress_off_message.pl` | D3: `pg_upgrade` says "being enabled" for a mid-*disable* cluster | no* |
| `t/103_data_checksums_enumvals.pl` | D4: `pg_settings.enumvals` is the phantom `{""}` | no |
| `t/104_progress_blocks_per_fork.pl` | D5: progress block counters are per-fork, documented per-relation | no |
| `t/105_launcher_kill_disable_wedge.pl` | D2: interrupted *disable* not self-healed (recoverable, not automatic) | yes |
| `t/106_checksum_func_grant_defeated.pl` | D6: granted `EXECUTE` defeated by internal `superuser()` gate | no |

\* `t/102` reaches a real `inprogress-off` state with SIGSTOP/SIGTERM, not an
injection point.

## Run

From a build of this branch (configure with `-Dtap_tests=enabled
-Dinjection_points=true`, installed to `$INST`):

```sh
PATH=$INST/bin:$PATH \
PG_REGRESS=$PWD/build/src/test/regress/pg_regress \
PERL5LIB=$PWD/src/test/perl \
enable_injection_points=yes \
    prove -v src/test/modules/test_checksums/audit-defect-tests/t/*.pl
```

Each script starts its own cluster (checksums off via `--no-data-checksums` where
it tests online enabling) with small `shared_buffers`; runtimes are a few seconds
each. On this branch every script ends `Result: FAIL` with its `# DEFECT:`
assertion(s) as `not ok` and its setup/context assertions as `ok`.
