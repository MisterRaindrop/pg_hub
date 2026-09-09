# Provenance

## Request

The user supplied this prompt verbatim on 2026-09-04 (UTC):

> Norms you must never violate:
> - In the norms below, replace $THIS_CONVERSATION_NAME with the name of this
>   conversation, the one that would be changed by /rename.
> - Use your own worktree(s); disregard the present dir except as repository to which to attach your worktree.
> - When you create a branch, prefix its name with: $THIS_CONVERSATION_NAME/
> - Never modify a branch you did not create.
> - Never modify a worktree you did not create.
> - When you create a directory or file in a directory you did not create, it
>   must have a name that starts with: $THIS_CONVERSATION_NAME
>
> Make a large workflow, with at most 270 agents, to write test cases covering
> user-visible defects in commit f19c0ec that are still present in branch
> f19c0ec-checksum-online.  Use your own worktree; disregard the present dir
> except as repository to which to attach your worktree.  The workflow should
> first look for extant user-visible defects.  If it finds any, write a test
> case covering some of those defects.  If any defects found weren't suitable to
> test, describe them in a report.
>
> Commit the following on a fresh branch:
> - A report describing any defects found, testable or not.  Prefix the report
>   with [no defects] if that's so.
> - Any tests written
> - A PROVENANCE.md file containing model, prompt, etc.

The supplied environment identified `/home/nm/src/pg/postgresql` as the source
repository, 2026-09-04 as the date, and UTC as the timezone.

## Model and orchestration

- Coordinator: OpenAI Codex using `gpt-5.6-sol` at `ultra` reasoning effort,
  as recorded in the session turn context.  The Codex CLI version was
  `0.152.0`.
- Subagents: 14 distinct Codex subagents (eight direct and six nested),
  inheriting the coordinator model, were used over more than twenty delegated
  discovery, reproduction, adversarial-review, history, and test-review
  assignments.  At most three subagents ran beside the coordinator at once,
  respecting the environment's four-slot concurrency limit and the user's
  270-agent ceiling.
- Conversation name at creation time: `f19c0ec-checksum-online`; renamed by
  the user to `f19c0ec-checksum-online-gpt` before branch creation.
- Session/thread identifier: `01a06e31-c5ad-7ac2-a537-3b041125c354`.
- Human input: the prompt and environment above, followed only by the
  conversation rename and a request to continue.  There was no human defect
  selection, code contribution, or review.
- Authorship: Codex generated `REPORT.md`, `PROVENANCE.md`, and both TAP test
  files.  Subagents contributed analysis and review but made no tracked edits;
  conceptual reuse from prior repository work is disclosed below.

The delegated workflow used these roles:

1. establish commit ancestry, scope, and later-fix history;
2. inspect checksum state transitions and background-worker lifecycles;
3. inspect SQL/GUC metadata, privilege behavior, and client tools;
4. inspect monitoring and documentation contracts;
5. inspect storage, WAL, temporary-relation, and progress behavior;
6. inspect replication, backup, restore, and upgrade interactions;
7. reproduce candidates dynamically and challenge their user visibility and
   `f19c0ec` attribution;
8. review new TAP tests independently for determinism, cleanup, portability,
   and assertion quality.

## Repository and isolation

- Introduction commit audited: `f19c0ec`.
- Target branch inspected without modification: `f19c0ec-checksum-online`.
- Target commit tested: `50bb165a05199c2cf93c1c9f42ddbca4162bee7a`.
- Private worktree created detached for the audit, then attached to the fresh
  branch `f19c0ec-checksum-online-gpt/defect-tests`:
  `/home/nm/src/pg/f19c0ec-checksum-online-worktree`.
- Private build, install, and run roots:
  `/home/nm/src/pg/f19c0ec-checksum-online-build`,
  `/home/nm/src/pg/f19c0ec-checksum-online-install`, and
  `/home/nm/src/pg/f19c0ec-checksum-online-test-run`.

The target branch name occupies Git ref `refs/heads/f19c0ec-checksum-online`.
Git therefore cannot simultaneously create a branch below
`refs/heads/f19c0ec-checksum-online/`, which the required conversation-name
prefix would demand.  Discovery and authoring proceeded detached so that the
existing branch was not renamed, deleted, or modified.  The user renamed the
conversation to `f19c0ec-checksum-online-gpt`, after which the fresh branch
`f19c0ec-checksum-online-gpt/defect-tests` was created from target commit
`50bb165a05199c2cf93c1c9f42ddbca4162bee7a`.  The object ID of the commit
containing this file is not embedded in the file itself, because doing so
would make the commit hash self-referential.

No branch or worktree owned by another workflow was modified.  All new tracked
files are below a newly created directory whose name begins with the
conversation name.

## Evidence and prior work

Primary evidence came from source and documentation at the target commit,
`git log`, `git blame`, PostgreSQL's existing TAP tests, and fresh dynamic
reproductions.

One subagent also consulted these external primary PostgreSQL sources for
context while evaluating replication and recovery leads:

- [PostgreSQL 19 documentation: Data Checksums](https://www.postgresql.org/docs/19/checksums.html);
- pgsql-hackers thread "Changing the state of data checksums in a running
  cluster": [message 1](https://www.postgresql.org/message-id/f1281cf3-89a3-4936-9bc5-2a5a6291229f@vondra.me),
  [message 2](https://www.postgresql.org/message-id/9e1331e1-93a0-4e27-934a-17b89342be4d@vondra.me),
  [message 3](https://www.postgresql.org/message-id/FAE6FC0E-AA0B-4CF4-B49B-BA6C2FC55FB8@yesql.se),
  and [message 4](https://www.postgresql.org/message-id/538e820b-db2a-4f53-ba24-c354c72fc1a9@vondra.me).

Those sources provided design context only and helped reject replication
candidates.  No counted finding, test logic, or copied report prose was
derived from them; the report's evidence is the checked-out tree, its history,
and live behavior.

During repository-wide discovery, the workflow found an existing read-only
branch `f19c0ec-checksum-defect-tests` at commit `fba2cb1`.  It was inspected
as prior art after independent source analysis and supplied several candidate
leads.  It was not checked out for modification or cherry-picked.  In
particular, its `t/103_data_checksums_enumvals.pl` identified the same enum
metadata defect and used the core `setting = ANY(enumvals)` consistency
invariant also used here.  The new, shorter `t/001_enumvals.pl` retains that
concept, requires all four states if the setting remains an enum, and permits
a legitimate fix that changes the read-only setting to a non-enum type.

This audit independently reclassified or rejected several of the prior
branch's other claims and found the late-temporary-table cutoff defect that
its tests did not cover.  The late-temp test and the revised enum test were
written in the private worktree rather than copied from the prior branch.

## Build and verification

The exact target was configured with Meson using assertions, debug support,
TAP tests, and injection points:

```text
CC='ccache gcc' meson setup <build> <source> --prefix=<install> \
  -Dcassert=true -Ddebug=true -Doptimization=1 \
  -Dtap_tests=enabled -Dinjection_points=true
ninja -C <build> install
meson test -C <build> --suite setup
meson test -C <build> test_checksums/001_basic --print-errorlogs
```

After the fresh branch was created, the final test blobs were run from
`post-branch-001` and `post-branch-002` directories below the private run
root.  Each `prove` process received exactly this relevant environment:

```text
PATH=/home/nm/src/pg/f19c0ec-checksum-online-build/tmp_install/home/nm/src/pg/f19c0ec-checksum-online-install/bin:/usr/bin:/bin
LD_LIBRARY_PATH=/home/nm/src/pg/f19c0ec-checksum-online-build/tmp_install/home/nm/src/pg/f19c0ec-checksum-online-install/lib/x86_64-linux-gnu
PERL5LIB=/home/nm/src/pg/f19c0ec-checksum-online-worktree/src/test/perl
top_builddir=/home/nm/src/pg/f19c0ec-checksum-online-build
PG_REGRESS=/home/nm/src/pg/f19c0ec-checksum-online-build/src/test/regress/pg_regress
REGRESS_SHLIB=/home/nm/src/pg/f19c0ec-checksum-online-build/src/test/regress/regress.so
INITDB_TEMPLATE=/home/nm/src/pg/f19c0ec-checksum-online-build/tmp_install/initdb-template
```

The invocations were:

```text
prove -v /home/nm/src/pg/f19c0ec-checksum-online-worktree/src/test/modules/test_checksums/f19c0ec-checksum-online-defect-tests/t/001_enumvals.pl
prove -v /home/nm/src/pg/f19c0ec-checksum-online-worktree/src/test/modules/test_checksums/f19c0ec-checksum-online-defect-tests/t/002_late_temp_table.pl
```

The three setup tests passed, and the existing `test_checksums/001_basic` TAP
test passed all 27 assertions.  With the temporary-install TAP environment:

- `t/001_enumvals.pl` failed only its intended assertion and reported
  `type=enum setting=off enumvals={""}`;
- `t/002_late_temp_table.pl` was repeated independently and consistently
  passed setup assertions 1-4, then failed only its intended final assertion
  with `blocked-by-late-temp` in about 7-8 seconds;
- Perl syntax checks and `git diff --check` passed;
- all test clusters and live-reproduction processes were stopped during
  cleanup.

The orphan-worker candidate was also reproduced in a fresh private cluster by
gating worker startup, pausing and resuming the postmaster with
`SIGSTOP`/`SIGCONT`, and terminating the launcher.  Its exact observation
is recorded in `REPORT.md`; no retained tracked fixture or server data came
from that experiment.

`pgperltidy` could not be run because the environment lacked its `perltidy`
dependency.  Independent style review found no actionable formatting issue.
