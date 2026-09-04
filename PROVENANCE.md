# PROVENANCE

Branch `f19c0ec-checksum-defect-tests` is the product of an automated, model-driven
audit of PostgreSQL commit `f19c0ec` ("Online enabling and disabling of data
checksums"). The report (`AUDIT-REPORT-checksum-online.md`), the six reproducer
tests, and this file were written by a language model orchestrating many
subagents. **No human wrote the report or the tests.** The repository owner
supplied the prompt, then reviewed and re-ran the results: he re-ran every test,
verified each defect's `git blame` ancestry himself, corrected one over-claim,
promoted one finding the automation had left "uncertain", and fixed a bug in one
test's error-handling. Those human interventions are listed under
[Human review](#human-review-what-the-automation-got-wrong).

## Tooling and model

* **Tool:** Claude Code (Anthropic's agentic CLI), using its Workflow orchestration
  feature — one JavaScript script driving many subagents through three phases
  (find → adversarially verify → write reproducer tests).
* **Model:** **Opus 4.8**, for the orchestrator and for every subagent (no
  per-agent model override, so the subagents ran under the session model). The
  session's default was initially set to Fable 5.1 (`claude-fable-5-1`) with
  `/model`, but the session was switched to Opus partway through (a "[cyber]"
  switch-to-Opus notice; the in-session `Co-Authored-By` identity changed to
  "Claude Opus 4.8"; and the usage was billed as Opus). The workflow run and the
  assembly therefore ran under Opus 4.8, which is also what the commit's
  `Co-Authored-By` trailer records. (An earlier draft of this file said Fable 5.1,
  taken from the assistant's system prompt; that does not reliably reflect the
  serving model and was corrected once the billing, the switch notice, and the
  co-author identity were reconciled.)
* **Run by:** the repository owner (Noah Misch), interactively, from
  `/home/nm/src/pg/postgresql`.
* **Workflow run:** `wf_9e53ff76-24e`. 47 subagents, 0 errors, ~136 minutes,
  ~5.39M subagent tokens / ~2.51M orchestrator-visible output tokens.

## The prompt

Verbatim, as typed by the repository owner:

```
Make a large workflow, with at most 90 agents, to write test cases covering
user-visible defects in commit f19c0ec that are still present in branch
f19c0ec-checksum-online.  Use your own worktree; disregard the present dir
except as repository to which to attach your worktree.  The workflow should
first look for extant user-visible defects.  If it finds any, write a test
case covering some of those defects.  If any defects found weren't suitable to
test, describe them in a report.

The workflow should adaptively manage usage so, when it reaches a session
limit, it loses 1-2 agents instead of 16 concurrent agents.  (Example that you
can improve on:  start at 16 concurrent, but when one exits, estimate the
burndown curve.  If there's a decent chance an agent started now would fail
due to session limit, don't replace the agent that just exited.  Check the
curve again at the next agent exit.)

Commit the following on a fresh branch:
- A report describing any defects found, testable or not.  Prefix the report
  with [no defects] if that's so.
- Any tests written
- A PROVENANCE.md file containing model, prompt, etc.
```

## Environment

* **Audited commit:** `f19c0ec` "Online enabling and disabling of data checksums"
  (Daniel Gustafsson, 2026-04-03).
* **Branch under audit:** `f19c0ec-checksum-online`, tip `50bb165`. That tip is the
  repository's `origin/master` (`6885b84`) plus one unrelated commit (`7344937`,
  parallel-scan atomics) and the four checksum follow-ups `05e7b3e`, `1d4d52d`,
  `85f7eed`, `50bb165`. So "present at the tip" means present after those four
  follow-ups.
* **This branch:** `f19c0ec-checksum-defect-tests`, based on `50bb165`, in a
  dedicated `git worktree` at `/home/nm/src/pg/csum-audit` attached to the owner's
  clone at `/home/nm/src/pg/postgresql`. The main working directory was used only
  as the repository to attach the worktree to, per the prompt.
* **Build:** meson, `-Dcassert=true -Ddebug=true -Doptimization=1
  -Dtap_tests=enabled -Dinjection_points=true`, `CC="ccache gcc"`, installed to
  `/home/nm/src/pg/csum-inst`.
* **Host:** Debian 13 (trixie), Linux 6.12.94 x86_64.
* **Harnesses (not part of this branch):** `/home/nm/src/pg/csumrun.sh` (throwaway
  SQL cluster, `initdb --no-data-checksums`, `psql -X`) and
  `/home/nm/src/pg/csumtap.sh` (standalone `PostgreSQL::Test` TAP against the
  install). Intermediate evidence lives outside the branch under
  `/home/nm/src/pg/csumwork` and the workflow transcript directory.

## Scope rubric given to every agent

A defect is behavior contrary to the feature's own binding principles — its
documentation, invariants stated in its commit message and code comments, plain
correctness/data-integrity, and consistency with how the rest of PostgreSQL
behaves in the equivalent situation — that is **user-visible** (SQL, a client
tool, a log line, monitoring output, a crash, a hang, corruption), **introduced by
`f19c0ec`**, and **still present at the tip**. Explicitly out of scope: anything
already fixed by the four follow-ups, anything already asserted by the shipped
tests `t/001`–`t/024`, and internal-only cleanliness.

## Method

1. **Find** (18 subagents, one per subsystem lens: the enable/disable state
   machine, launcher/worker lifecycle, progress view, checksum-failure stats, the
   `data_checksums` GUC, `pg_control`/SRFs, `pg_upgrade`, logical decoding, base
   backup, the read/write verify paths, single-node crash/restart, `pg_waldump`
   desc, wait events, and a docs-vs-behavior sweep). Read-only analysis of the tip,
   returning structured candidates. → 12 candidates.
2. **Verify** (2 subagents per candidate = 24): a **reproducer** that builds and
   runs a probe on the installed build, and a **skeptic** prompted to refute by
   default (pre-existing? already fixed? already tested? internal-only? documented
   behavior actually matches?). → 5 confirmed, 3 uncertain, 4 refuted.
3. **Test** (one subagent per confirmed, testable defect = 5): write a standalone
   TAP reproducer and iterate until it runs cleanly and fails on this branch for
   the right reason.

The six committed tests are the five the workflow wrote (`t/101`–`t/105`) plus one
(`t/106`) the owner added while promoting an "uncertain" finding (see below).

## Adaptive usage control (the prompt's second requirement)

A single controller governed all three fan-out phases (`adaptiveRun` in the
workflow script). It answers the prompt's requirement — lose 1-2 agents at a
session limit, not 16 — with two mechanisms:

* **Soft ramp.** A shared token baseline is captured once; as spend approaches an
  estimated one-window ceiling (`CEILING_DELTA = 3,000,000` output tokens,
  best-effort — the true session limit is not readable from the script and is
  shared across concurrent sessions), the per-exit concurrency target shrinks, but
  **never below one**, so a phase always finishes. Overshooting the true limit
  while already at concurrency one loses at most that one agent.
* **Hard-failure backstop.** The first agent that dies (a `null` return, the signal
  a real limit gives) caps the target at 2; the second caps it at 1; a third drains
  in-flight work and pauses, to be resumed from cache. This is what bounds the loss
  to 1-2 even if the soft estimate is wrong.

The target is recomputed at every agent exit — the prompt's "check the curve again
at the next agent exit."

**How it actually behaved this run** (from the workflow's own progress log): Find
started at the requested 16. After the first of 16 concurrent finders returned, the
shared spend already stood at 483k, so the running per-agent average was
momentarily huge and the controller conservatively dropped the target to 3; as more
finders completed and the average settled, it recovered to 15-16 and Find finished
all 18. Verify ran at its cap of 4, Test at 3 (both lowered from 16 on purpose, to
bound concurrent live clusters on a 2-CPU / ~2 GB-RAM host, independent of the
session-limit logic). No agent ever failed (`fails 0` throughout), total spend
(~2.51M) stayed under the ceiling, so the hard-failure backstop and the pause/resume
path were implemented and exercised in the ramp but never triggered by a real limit.
The early dip to target 3 is an artifact of measuring average cost from a shared,
cumulative token counter while many agents are in flight; it is conservative (it
throttles rather than over-commits) and self-corrected.

## Human review — what the automation got wrong

The owner reviewed the automation's output rather than trusting it, and changed
three things:

* **D2 over-claim corrected.** The automation titled the interrupted-disable
  finding "permanently wedges the cluster in `inprogress-off`." Its own test
  already showed the state is recovered by re-issuing `pg_disable_data_checksums()`
  (and the docs note a restart resolves it). "Permanently" was removed from the
  report and from the test's comments; the defect is the *asymmetry* (an
  interrupted enable self-heals, an interrupted disable does not), not a permanent
  wedge.
* **D6 promoted from "uncertain".** The skeptic had refuted the GRANT/`superuser()`
  finding as defensible design. On review it is a genuine, if low-severity,
  inconsistency: the `{POSTGRES=X}` ACL is delegable and `has_function_privilege`
  returns true, yet the grant is silently ineffective — unique among `{POSTGRES=X}`
  functions. It was promoted to a confirmed defect and given a new, fix-agnostic
  test (`t/106`), written and verified by the owner during review.
* **A test bug was fixed.** `t/106`'s first draft used `on_error_stop => 0` when
  shelling out as the granted role, so a SQL error left `psql`'s exit status at 0
  and the helper reported success; the test passed spuriously. The helper was
  changed to detect the error on stderr, after which the test correctly fails at
  its defect assertions.

The owner also verified independently, by `git blame` and by diffing the four
follow-ups, that each confirmed defect is attributable to `f19c0ec` and untouched
by the follow-ups; and re-ran all six tests against this branch, confirming each
fails only at its `# DEFECT:` assertions.

One further real problem — a stale "Replication" paragraph in `wal.sgml` — was
found but **excluded**: the paragraph was accurate for `f19c0ec` and became wrong
only when follow-up `05e7b3e` changed standby replay, so it is a follow-up defect,
not `f19c0ec`'s. It is described in the report for completeness.

## What is on this branch

```
AUDIT-REPORT-checksum-online.md                                  the report (6 defects + 1 out-of-scope + 4 refuted)
PROVENANCE.md                                                    this file
src/test/modules/test_checksums/audit-defect-tests/README.md     how to run the tests
src/test/modules/test_checksums/audit-defect-tests/t/101..106     six reproducer TAP tests
```

The tests are intentionally not wired into the `meson`/`make` suite: this branch
carries no fix, so they fail by design, and a correct fix for each defect turns its
failing assertion green.
