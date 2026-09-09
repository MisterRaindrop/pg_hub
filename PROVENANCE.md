# PROVENANCE

Branch `dump-sort-stability-tests` is the output of an automated, model-driven audit that
looked for sources of pg_dump dump-order instability **other than** the one fixed by the
cast/transform patch the branch's first commit carries.  Everything on the branch after
that first commit -- the report, the regression tests, and the SAMPLE fixes -- was written
by a language model.  **No human wrote any of it, and no human has reviewed it.**  This
file records how it was produced, including what went wrong, so a reviewer can judge the
work and reproduce every claim in it.

---

## Tooling

* **Tool:** Claude Code (Anthropic's agentic CLI), using its Workflow feature -- a
  deterministic JavaScript script that drives many subagents.
* **Model:** Opus 5 (`claude-opus-5`) for the orchestrator and for every subagent.
* **Run by:** the repository owner (Noah Misch), interactively, from
  `/home/nm/src/pg/postgresql`.
* **Date:** 2026-09-03.
* **Human contribution:** the prompt below, and one mid-run question about whether work was
  blocked.  Nothing in the findings, the tests, the fixes or the report came from a human.

## The prompt

> ~/sort_cast.patch contains a reasonable-looking fix.  I'm concerned that yet
> more sources of dump sort instability are still lurking.  Make a worklfow to
> look for others and, if any found, write test cases covering them.  Use your
> own worktree; disregard the present dir except as repository to which to
> attach your worktree.
>
> Commit the following on a fresh branch:
> - A report.  Prefix the report with [no defects] if that's so.
> - Any tests written
> - A PROVENANCE.md file containing model, prompt, etc.

## Base

* Upstream `master` at `6885b84` ("doc: Fix link on pg_dsm_registry_allocations page.").
* Commit `885a841` is `~/sort_cast.patch` applied verbatim with `git am`.  It is Alexander
  Kukushkin's patch, **not** model-written; it is on the branch because the audit's whole
  purpose was to find what that patch does not cover.  Everything after it is the audit.

## What was built to do the audit

Three artifacts, all outside the branch, under `/home/nm/src/pg/`:

* `ss-audit/` -- the branch worktree.  Agents were given it **read-only**.
* `ss-inst/` -- a stock assert-enabled install of `885a841`.  Its `pg_dump` aborts when
  `DOTypeNameCompare()` reaches `Assert(false)`, which is the audit's primary oracle
  because it is exactly what a developer or buildfarm animal sees.
* `ss-instr/` + `ss-shuf-inst/` -- the same commit with audit-only instrumentation in
  `sortDumpableObjectsByTypeName()`: a pre-sort shuffle under `PGDUMP_SHUFFLE_SEED`, a
  complete adjacent-pair tie report under `PGDUMP_TIE_REPORT`, and the comparator's
  `Assert(false)` disarmed (which also makes it a faithful stand-in for a production
  non-assert build).  The instrumentation is reproduced in full in the report's appendix.
  It was **never** committed to the branch.
* `ssrun/pgrun.sh` -- runs a `.sql` file in a throwaway cluster and dumps it in `plain`,
  `tie` or `shuffle` mode.  `ssrun/regdata/` -- a prepared core-regression database
  (243/243 tests passed, 2291 relations) used as the large realistic corpus.

Calibration before any agent ran: all three oracles fire on a known defect and are silent
on the regression database and on control schemas.

## What the agents did

**Workflow 1 -- discovery** (`wf_7ac43658-438`, 152 agents, 110 completed, 10.0M subagent
tokens, 8h04m wall clock).

* 17 discovery agents in parallel: 8 sweeping the 48 `DumpableObjectType` values against
  their catalogs' natural keys, 4 code lenses (the topological sort; every dump-time query
  in `pg_dump.c`; `pg_dumpall.c` and the archive TOC; the history of the five commits that
  already fixed this class), and 5 empirical lanes (cross-schema name collisions; pg_dump's
  manufactured pseudo-objects; in-tree extensions; the regression corpus; a differential
  two-database generator).
* 54 raw candidates, deduplicated to 49 by key.
* Each candidate then got an independent verification agent, and each survivor an
  independent adversarial judge whose brief was to **refute** it.  40 survived.
* Those 40 describe **11 distinct mechanisms**; one mechanism was found independently by 13
  different agents through 13 different object types.

**Workflow 2 -- tests and fixes** (`wf_db8fc3d2-7db`, 12 agents, 12 completed, 1.8M
subagent tokens): one agent per confirmed mechanism, each required to re-reproduce it from
scratch before writing its regression test and sample fix, plus two adversarial critics --
one told to falsify the claim that `DO_POLICY` is the only remaining comparator tie, one to
settle a finding the first workflow's judges had split on.

The first critic returned **"claim holds"** after enumerating all 48 `DumpableObjectType`
values and their 55 construction sites and attacking the claim with five SQL corpora, each
under 17 pg_dump option sets, plus all 59 contrib extensions and the regression database.
The second **found a twelfth defect** (D12) while refuting the finding it was sent to
adjudicate: the two array-order cases it was given are not defects, but the sweep had
stopped one array short of `pg_default_acl.defaclacl`, which is.  So the final count is
**twelve**, not the eleven the discovery workflow produced.

## What went wrong, and what was done about it

* **The Anthropic API returned 529 Overloaded for about an hour.**  It killed workflow 1's
  entire Tests phase and its completeness critic (42 of the 152 agents), then two full
  launches of workflow 2 (12 agents each, all failing at zero tokens).  **No finding was
  lost** -- discovery, verification and adjudication had all completed -- but no test was
  written until the third launch of workflow 2 succeeded.
* **The first tie-detector build was wrong and reported nothing.**  Disarming
  `Assert(false)` left the fall-through returning `oidcmp()`, so the reporter's
  "did these two compare equal?" test never fired.  Caught by running it against a defect
  known to be present; fixed by having the fall-through set a flag the reporter reads.
  Recorded here because a silent detector is the failure mode that would have made this
  whole audit report "no defects".
* **The shuffle oracle produced false positives** until `pg_dump`'s random `\restrict`
  token was normalized away before diffing.
* **The first dedup was too coarse**: 40 confirmed reports collapse to 11 mechanisms, but
  the agents chose 40 different key strings, so the Tests phase was sized at 42 agents when
  10 would do.  Consolidation was done by hand between the two workflows.
* **`002_pg_dump.pl` cannot express every finding.**  Which findings got a TAP test, which
  did not, and why, is stated explicitly in the report -- no finding is quietly dropped.

## How to check the work

Every finding in the report carries the minimal SQL that produces it.  For a comparator
tie, run it under an assert-enabled `pg_dump` and watch the assertion fire.  For an
ordering finding, build the two databases the report gives and diff the dumps.  The
tests on this branch are the same reproducers expressed in `002_pg_dump.pl`; each one
fails on `885a841` and passes with that finding's sample fix applied.

The audit's own verification of the committed artifacts is described at the end of the
report, including the result of reverting the sample fixes and re-running the suite.
