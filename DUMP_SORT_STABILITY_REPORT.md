# pg_dump dump-order stability: audit of what `sort_cast.patch` does not cover

**Question asked:** after the DO_CAST / DO_TRANSFORM tiebreakers, are more sources of
dump-order instability still lurking?

**Answer:** yes -- **twelve**, but only **one** of them is another tie in
`DOTypeNameCompare()`.  The other eleven are outside the object sort: one in dependency-loop
repair, ten in dump-time queries and emitters whose result order reaches the output text
directly.  One of those is not merely an ordering nuisance: it makes a plain `pg_dump`
&rarr; restore **silently permute an inherited table's column order**.

| # | Where | What | Severity |
|---|---|---|---|
| D1 | `DOTypeNameCompare()` | the RLS-enabled pseudo-object ties with a policy named after its own table -- `pg_dump` **aborts** on an assert build | high |
| D2 | `pg_dump_sort.c` loop repair | `TopoSort()` builds its failure list in dumpId order, so which object gets broken out of a dependency cycle follows OID assignment | medium |
| D3 | `getInherits()` | no `ORDER BY`; `inhseqno` is never read at all, so the `INHERITS` list follows heap order -- **restores with the child's columns permuted** | high |
| D4 | `dumpOpfamily()`, `dumpOpclass()` | member lists ordered only by `amopstrategy` / `amprocnum`, which is not a key | medium |
| D5 | `getPolicies()` | the policy `TO` role list is built by an unordered sub-select | medium |
| D6 | `getPublications()` | `FOR ALL TABLES EXCEPT (...)` list has no `ORDER BY` | medium |
| D7 | `dumpDatabaseConfig()` | `ALTER ROLE ... IN DATABASE ... SET` lines have no `ORDER BY` | medium |
| D8 | `append_depends_on_extension()` | `DEPENDS ON EXTENSION` lines have no `ORDER BY` | low |
| D9 | `collectSecLabels()`, `dumputils.c` | `ORDER BY` omits `provider`, which is part of the key | low |
| D10 | `pg_dumpall.c` `dumpTablespaces()` | `ORDER BY 1` on `SELECT oid, spcname, ...` sorts by **OID** | low |
| D11 | `dumpExtension()` `--binary-upgrade` | the `requires` array is emitted in dependency-array order, which is OID-derived | low |
| D12 | `getDefaultACLs()` | `defaclacl` is emitted in the backend's canonical **grantee-OID** order | medium |

Every one of the twelve was reproduced twice: once by an agent that found it, once by an
independent agent whose brief was to refute it.  D1, D3, D4, D2 and D12 were additionally
reproduced by hand, outside the agent framework; the commands are in this report.

A twelfth candidate class -- `pg_dump` reproducing the element order of array-valued
catalog columns (`aclitem[]`, `setconfig`, `reloptions`) -- was **rejected**, and the
argument that killed it is worth reading: see [Considered and rejected](#considered-and-rejected).

---

## Method

Three oracles, in increasing order of reach.

**1. The stock assert-enabled build.** `DOTypeNameCompare()`'s fall-through is
`Assert(false)`, so on an assert build a tie makes `pg_dump` abort.  This is the oracle
that matters most, because it is exactly what a developer or a buildfarm animal sees, and
it needs no instrumentation.

**2. A tie reporter.** An audit-only build (`ss-shuf-inst`) whose
`sortDumpableObjectsByTypeName()` walks the sorted array afterwards and reports every
adjacent pair for which the comparator reached the fall-through, with
`describeDumpableObject()` output for both.  Ties are necessarily adjacent after a sort, so
one run enumerates *all* of them rather than aborting at the first.  The `Assert` is
disarmed in that build, which also makes it a faithful stand-in for a production build:
with no environment variables set it falls through to `oidcmp()` exactly as a non-assert
`pg_dump` does.

**3. A pre-sort shuffle.** The same build permutes the object array before sorting when
`PGDUMP_SHUFFLE_SEED` is set, and makes the fall-through return 0 rather than comparing
OIDs, so a tie leaves the order genuinely up to `qsort`.  Eight seeds, then diff.  Dump
output must not depend on the input permutation; if it does, the order is unstable --
whatever the cause, including causes that never reach the comparator.

Calibration: all three fire on the DO_POLICY case (D1) and all three are silent on the
core regression database (2291 relations) and on control schemas.  The shuffle oracle is
blind to the D3..D11 class by construction -- those orders come from the *server's* result
order, which is identical under every seed -- so that class had to be found by reading the
queries and confirmed by diffing two independently built databases.  Where a finding is of
that kind, the report says so explicitly and gives the two-database pair.

**Fan-out.** A first workflow ran 17 discovery agents -- 8 sweeping the 48
`DumpableObjectType` values against their catalogs' natural keys, 4 code lenses (the
topological sort; every dump-time query in `pg_dump.c`; `pg_dumpall.c` and the archive TOC;
the history of the five commits that already fixed this class), and 5 empirical lanes
(cross-schema name collisions for every schema-qualified object type; pg_dump's
manufactured pseudo-objects; in-tree extensions; the regression corpus; a
differential two-database generator).  Their 54 raw candidates deduplicated to 49, each of
which got an independent verification agent and, unless refuted, an independent adversarial
judge told to refute it.  40 survived; those 40 describe **11 distinct mechanisms** --
the same defect was found by up to 13 agents through different object types.  A second
workflow put one agent on each surviving mechanism to re-reproduce it from scratch, write
its regression test and write a sample fix, plus two adversarial critics.

**What this method cannot see.** Dump-order instability that requires a server version
older than this tree (`pg_dump` supports back to 9.2 and builds several queries in
version-dependent branches; only the modern branch was exercised), instability visible only
under `pg_restore -j` scheduling, and anything needing a platform this box is not.

---

## Part 1 -- the object sort

### D1. `DO_POLICY`: the RLS-enabled pseudo-object collides with a policy named after its table

`getPolicies()` represents "row level security is enabled on this table" as a `PolicyInfo`
with `polname == NULL`, and gives it the **table's** name:

```c
/* src/bin/pg_dump/pg_dump.c:4249 */
polinfo->dobj.objType = DO_POLICY;
polinfo->dobj.catId.tableoid = 0;
polinfo->dobj.catId.oid = tbinfo->dobj.catId.oid;
AssignDumpId(&polinfo->dobj);
polinfo->dobj.namespace = tbinfo->dobj.namespace;
polinfo->dobj.name = pg_strdup(tbinfo->dobj.name);   /* <-- borrowed */
polinfo->poltable = tbinfo;
polinfo->polname = NULL;
```

A real policy gets `dobj.name = polname` and the same namespace, and the `DO_POLICY`
tiebreaker compares only the table name:

```c
/* src/bin/pg_dump/pg_dump_sort.c */
else if (obj1->objType == DO_POLICY)
{
	/* Sort by table name (table namespace was considered already) */
	cmpval = strcmp(pobj1->poltable->dobj.name, pobj2->poltable->dobj.name);
	if (cmpval != 0)
		return cmpval;
}
```

So for a policy whose `polname` equals its own table's `relname`, every step returns 0:
same priority, same namespace, same name, same objType, same table.  This is the same shape
as the cast/transform defect -- a name that is not the object's own -- but arrived at from
the other direction: instead of building a name out of other objects' unqualified names, it
*borrows* one wholesale.

Three statements reproduce it:

```sql
CREATE TABLE pol_t (i int);
ALTER TABLE pol_t ENABLE ROW LEVEL SECURITY;
CREATE POLICY pol_t ON pol_t USING (true);
```

```
$ /home/nm/src/pg/ssrun/pgrun.sh plain t_policy.sql >/dev/null
pg_dump: pg_dump_sort.c:511: DOTypeNameCompare: Assertion `0' failed.
PG_DUMP FAILED (exit 134)
ASSERTION FAILURE

$ /home/nm/src/pg/ssrun/pgrun.sh tie t_policy.sql >/dev/null
SORT TIE: objType 41 name "pol_t" nsp "public" | POLICY (ID 3523 OID 16384) | POLICY (ID 3524 OID 16387)
TIES DETECTED
```

**On an assert-enabled build the database is simply not dumpable.**  On a production build
the tie falls through to `oidcmp()`, comparing the *table's* OID (the pseudo-object carries
`catId.oid = table oid`, `tableoid = 0`) against the *pg_policy* OID.  In a
normally-built database the policy always postdates its table, so the order is stable by
luck.  It stops being stable exactly where `pg_upgrade` operates: relation OIDs are
preserved across an upgrade, `pg_policy` OIDs are not.  Give the table a high OID in the
old cluster and the restored policy gets a low one, and the two logically identical
databases dump in opposite orders:

```
== old cluster OIDs ==            == new cluster OIDs ==
 table  | 18784                    table  | 18784   (preserved)
 policy | 18787                    policy | 16384   (reassigned)

$ diff -u old.dump new.dump
--- Name: rls_demo; Type: ROW SECURITY; Schema: public; Owner: postgres
+-- Name: rls_demo rls_demo; Type: POLICY; Schema: public; Owner: postgres
-ALTER TABLE public.rls_demo ENABLE ROW LEVEL SECURITY;
+CREATE POLICY rls_demo ON public.rls_demo USING (true);
```

(`/home/nm/src/pg/ssrun/oidflip.sh` -- it pads the OID counter, takes a
`pg_dump --binary-upgrade --schema-only`, restores it into a second cluster started with
`-b`, and dumps both with the assert-disarmed build.  That is the flake the comment above
`Assert(false)` predicts, reproduced deliberately.)

The fix is the missing natural-key column.  `pg_policy`'s key is `(polrelid, polname)`; the
pseudo-object is the one row where `polname` is absent, so comparing "is `polname` NULL"
after the table name completes the key:

```c
	/*
	 * The RLS-enabled pseudo-object (polname == NULL) borrows its name from
	 * its table, so it ties with a policy whose polname equals that table
	 * name.  Sort the pseudo-object first, consistent with ENABLE ROW LEVEL
	 * SECURITY logically preceding the policies on the table.
	 */
	if (pobj1->polname == NULL)
	{
		if (pobj2->polname != NULL)
			return -1;
	}
	else if (pobj2->polname == NULL)
		return 1;
```

Two non-NULL `polname`s on the same table cannot both survive to this point: `polname`
*is* `dobj.name`, already compared at step 3.

### Why nothing else in the comparator ties

D1 is the only tie the audit found, and that claim was put to a dedicated adversarial
critic whose brief was to falsify it.  The reason the rest of the comparator is sound comes
down to two observations that are worth recording, because they are what a future reviewer
needs in order to check a new object type:

1. **Constructed names are now closed.**  Only three construction sites build a
   `dobj.name` out of other names rather than copying a catalog column: `getCasts()` and
   `getTransforms()` (fixed by `885a841`) and `getLOs()`, whose name is a large-object OID
   range -- and a large object's OID *is* its identity, so that one is not a defect.
2. **Borrowed names are safe wherever the borrower has its priority to itself.**  Twelve
   object types take their name from another object -- `DO_TABLE_ATTACH`,
   `DO_INDEX_ATTACH`, `DO_ATTRDEF`, `DO_TABLE_DATA`, `DO_SEQUENCE_SET`,
   `DO_REFRESH_MATVIEW`, `DO_REL_STATS`, `DO_SHELL_TYPE`, `DO_DUMMY_TYPE`,
   `DO_PUBLICATION_REL`, `DO_PUBLICATION_TABLE_IN_SCHEMA`, `DO_SUBSCRIPTION_REL` -- and
   every one of them is either alone at its priority level or separated from its
   priority-mate by the `objType` comparison, and each has at most one instance per
   borrowed-from object.  `DO_POLICY` is the single case where a borrowed-name
   pseudo-object shares both a priority *and* an `objType` with a genuinely named object.

Two near misses are worth a note rather than a change, and both were refuted with
structural arguments rather than merely not reproduced:

* `DO_INDEX` takes its namespace from its table rather than from `pg_class.relnamespace`,
  and has no tiebreaker.  Today an index's `relnamespace` is pinned to its table's, so
  `(namespace, name)` still reduces to `pg_class_relname_nsp_index`; the sort key is one
  line narrower than the natural key, but nothing can exploit it.
* the pseudo-objects built with `catId.tableoid = 0, catId.oid = 0`
  (`DO_TABLE_ATTACH`, `DO_INDEX_ATTACH`, `DO_REL_STATS`) have no OID for the
  `oidcmp()` safety net to fall back on, so if a future change did introduce a tie among
  them, the fall-through would return 0 and the order would be pure `qsort` luck rather
  than merely OID-dependent.

---

## Part 2 -- dependency-loop repair

### D2. `TopoSort()` reports its failures in dumpId order, so loop repair follows OID assignment

When the dependency graph has a cycle, `TopoSort()` fails and hands the objects it could
not place to `findDependencyLoops()`, which finds a cycle and calls
`repairDependencyLoop()` to break it -- by marking one object `separate`, so that (for
example) a `CHECK` constraint moves out of `CREATE TABLE` into a post-data
`ALTER TABLE ... ADD CONSTRAINT`, or one view of a mutually-recursive pair is emitted as a
dummy `SELECT NULL::...` placeholder and rebuilt later with `CREATE OR REPLACE VIEW`.

Which object gets chosen is decided by OID assignment order, not by name.  Three links:

```c
/* pg_dump_sort.c:757 -- the failure list is rebuilt in dumpId order, discarding
 * the name-sorted order the caller passed in */
k = 0;
for (j = 1; j <= maxDumpId; j++)
{
	if (beforeConstraints[j] != 0)
		ordering[k++] = objs[idMap[j]];
}
```

`findDependencyLoops()` then walks that array front to back, so `loop[0]` is the
lowest-dumpId cycle member; and `repairDependencyLoop()`'s multi-object branches scan
`loop[]` front to back and repair the *first* member of the type they are looking for.
dumpIds are handed out by `AssignDumpId()` in catalog-scan order, and the scans are
OID-ordered (`getTables()` ends `ORDER BY c.oid`; `getTypes()` and `getFuncs()` have no
`ORDER BY` at all, so heap order).  The whole repair decision therefore rides on which
object was created first.

Four statements, differing only in which of two domains is created first:

```sql
-- A                                    -- B
CREATE DOMAIN d1 AS int;                CREATE DOMAIN d2 AS int;
CREATE DOMAIN d2 AS int;                CREATE DOMAIN d1 AS int;
ALTER DOMAIN d1 ADD CONSTRAINT c1 CHECK ((CAST(VALUE AS int)::d2) IS NOT NULL);
ALTER DOMAIN d2 ADD CONSTRAINT c2 CHECK ((CAST(VALUE AS int)::d1) IS NOT NULL);
```

```
$ diff -u a.dump b.dump
-CREATE DOMAIN public.d2 AS integer
-	CONSTRAINT c2 CHECK (((VALUE)::public.d1 IS NOT NULL));
+CREATE DOMAIN public.d1 AS integer
+	CONSTRAINT c1 CHECK (((VALUE)::public.d2 IS NOT NULL));
-ALTER DOMAIN public.d1
-    ADD CONSTRAINT c1 CHECK (((VALUE)::public.d2 IS NOT NULL));
+ALTER DOMAIN public.d2
+    ADD CONSTRAINT c2 CHECK (((VALUE)::public.d1 IS NOT NULL));
```

A puts `c1` in a separate `ALTER DOMAIN` and inlines `c2`; B does the opposite.  No
assertion fires; the divergence is silent.  The verification agent checked that the two
databases are logically identical by projecting the whole catalog -- including the entire
`pg_depend` graph with every OID rendered as `regclass`/`regprocedure`/`regtype` -- and
diffing: no output.  The same instability was demonstrated through four different repair
paths (a table `CHECK` constraint via `BEGIN ATOMIC` functions, a domain `CHECK`
constraint, the dummy-view choice in a view/rule cycle, and which column `DEFAULT` is split
into a separate `ALTER TABLE ... SET DEFAULT`), and in one variant the dump flipped with
*every OID identical* -- so dumpId order, not OID order as such, is the real input.

**No sample fix is proposed for D2 and no test is committed for it.**  The natural fix has
two parts -- make `TopoSort()`'s failure list inherit the caller's name-sorted order, and
make `repairDependencyLoop()` pick the minimum by natural key rather than the first in
`loop[]` order -- and both change which object gets broken out in existing cases, i.e. they
change dump output for databases that dump fine today.  That is a judgement call about
`pg_dump`'s output, not a mechanical key completion, so it is written up here and left to
you.  A test pinned to today's choice would only entrench the OID dependence; a test
pinned to the fixed choice presumes the fix.

---

## Part 3 -- dump-time queries whose result order reaches the output

Nine of the eleven findings are of one shape: a query whose rows are pasted into the dump
in result order, ordered by less than a key -- or not ordered at all.  None of them reaches
`DOTypeNameCompare()`, so the tie and shuffle oracles are silent on all nine; each was
established by reading the query, checking the plan, and diffing two independently built
databases.  They are listed worst first.

### D3. `getInherits()` never reads `inhseqno`, and this permutes columns on restore

```c
/* src/bin/pg_dump/pg_dump.c:7696 */
appendPQExpBufferStr(query, "SELECT inhrelid, inhparent FROM pg_inherits");
```

No `ORDER BY`, and `pg_dump` reads `inhseqno` **nowhere** -- `grep -rn inhseqno
src/bin/pg_dump/` returns nothing.  `flagInhTables()` appends parents in `PGresult` order
(`common.c:323`) and nothing re-sorts, so the `INHERITS (...)` list at `pg_dump.c:17454`
and the `--binary-upgrade` `ALTER TABLE ONLY ... INHERIT` at `pg_dump.c:17764` both follow
`pg_inherits` **heap** order.  `pg_inherits`'s natural key is `(inhrelid, inhseqno)`.

Heap order diverges from `inhseqno` order as soon as a line pointer is reused, and it also
just differs with creation order when other children's rows are interleaved.  Seven
statements, all ordinary DDL:

```sql
CREATE TABLE p1 (a int);
CREATE TABLE p2 (b int);
CREATE TABLE decoy () INHERITS (p1);
CREATE TABLE ch (b int) INHERITS (p1);
DROP TABLE decoy;
VACUUM pg_inherits;
ALTER TABLE ch INHERIT p2;
```

The catalog now says the parent order is `p1` then `p2`, and `ch`'s columns are `(a, b)`
accordingly, but the two rows sit in the heap the other way round:

```
 ctid  | inhparent | inhseqno            attnum | attname
-------+-----------+----------          --------+---------
 (0,1) | p2        |        2                 1 | a
 (0,2) | p1        |        1                 2 | b
```

and `pg_dump` emits the heap order:

```sql
CREATE TABLE public.ch (
    b integer
)
INHERITS (public.p2, public.p1);
```

Restoring that gives `ch` the columns of `p2` first.  **The column order changes:**

```
== ORIGINAL ch columns:        == RESTORED ch columns:
 1 | a                          1 | b
 2 | b                          2 | a
```

This is not a spurious-schema-diff problem.  A restored database in which a table's columns
have swapped positions breaks `SELECT *`, `INSERT` without a column list, and every client
that binds by position -- silently, with no error anywhere in the dump or the restore.
(`pg_dump`'s own `COPY` statements carry explicit column lists, so the *data* lands in the
right columns; it is the schema that moves.)  Ordering the query by `(inhrelid, inhseqno)`
fixes both the instability and the wrong restore, and needs no other change because
`flagInhTables()` preserves `PGresult` order.

### D4. `dumpOpfamily()` and `dumpOpclass()` order members by strategy alone

```c
/* pg_dump.c, dumpOpfamily(): both member queries */
... "ORDER BY amopstrategy",     /* pg_amop  */
... "ORDER BY amprocnum",        /* pg_amproc */
```

`pg_amop`'s key is `(amopfamily, amoplefttype, amoprighttype, amopstrategy)` and
`pg_amproc`'s is `(amprocfamily, amproclefttype, amprocrighttype, amprocnum)`.  Within one
family, every cross-type member pair shares a strategy number, so the sort key is not a
key at all and the remaining order is the executor's -- which the judge traced to an index
scan on `pg_depend`, i.e. ascending member OID.  Adding the same two support functions in
the opposite order permanently changes the dump:

```sql
CREATE OPERATOR FAMILY myfam USING btree;
ALTER OPERATOR FAMILY myfam USING btree ADD FUNCTION 1 btint4cmp(int4, int4);
ALTER OPERATOR FAMILY myfam USING btree ADD FUNCTION 1 btint8cmp(int8, int8);
-- versus the same two ADDs in the opposite order
```

```
$ diff -u a.dump b.dump
 ALTER OPERATOR FAMILY public.myfam USING btree ADD
-    FUNCTION 1 (integer, integer) btint4cmp(integer,integer) ,
-    FUNCTION 1 (bigint, bigint) btint8cmp(bigint,bigint);
+    FUNCTION 1 (bigint, bigint) btint8cmp(bigint,bigint) ,
+    FUNCTION 1 (integer, integer) btint4cmp(integer,integer);
```

Deterministic, and it reproduces on every run.  The `pg_amop` half of the same query pair
has the identical missing key columns; the audit could not make the operator list flip
(the plan it gets happens to be insensitive to insertion order), so that half is reported
as latent rather than demonstrated.  `dumpOpclass()`'s two queries are also latent for a
different reason: only members whose left and right types both equal `opcintype` depend on
the opclass rather than the family, so today at most one member per strategy reaches them
-- access methods without an `amadjustmembers` hook are where that could stop holding.

### D5. `getPolicies()` builds the `TO` role list with an unordered sub-select

```c
/* pg_dump.c:4278 */
"CASE WHEN pol.polroles = '{0}' THEN NULL ELSE "
"  pg_catalog.array_to_string(ARRAY(SELECT pg_catalog.quote_ident(rolname) "
"                                   from pg_catalog.pg_roles "
"                                   WHERE oid = ANY(pol.polroles)), ', ') END AS polroles, "
```

The `ARRAY()` sub-select has no `ORDER BY` and plans as a seq scan on `pg_authid`, so the
list follows role **creation** order -- neither the stored `polroles` array order (which
`policy_role_list_to_array()` preserves from the `CREATE POLICY` text) nor `rolname` order.
Two databases whose roles were created in the opposite order dump
`CREATE POLICY p ON t TO alice, bob` versus `... TO bob, alice`.  Ordering by each OID's
position within `pol.polroles` (`unnest ... WITH ORDINALITY`) both stabilises it and makes
the clause a faithful round trip of what the user wrote.

### D6. `getPublications()` does not order the `FOR ALL TABLES EXCEPT` list

```c
/* pg_dump.c:4598, per publication, remoteVersion >= 190000 */
"SELECT prrelid\n"
"FROM pg_catalog.pg_publication_rel\n"
"WHERE prpubid = %u AND prexcept"
```

The rows go into a `SimplePtrList` in arrival order and `dumpPublication()` walks it
verbatim, so `CREATE PUBLICATION p FOR ALL TABLES EXCEPT (TABLE ONLY a, TABLE ONLY b)`
follows `pg_publication_rel` heap order -- i.e. the order the tables were listed when the
publication was created.  Three statements per database reproduce it.  This one is new
code (v19), which makes it the cheapest of the nine to fix before it ships in a release.

### D7. `dumpDatabaseConfig()` does not order per-role database settings

```c
/* pg_dump.c:3764 */
"SELECT rolname, unnest(setconfig) FROM pg_db_role_setting s, pg_roles r "
"WHERE setrole = r.oid AND setdatabase = '%u'::oid"
```

One row per `(role, database)`, no `ORDER BY`, and the plan seq-scans `pg_authid` on the
probe side, so the `ALTER ROLE ... IN DATABASE ... SET` lines in a `--create` preamble come
out in role-OID order.  `ORDER BY 1` (`rolname`) is a complete key here, and the
verification agent checked the one thing that could have gone wrong -- that sorting above
the set-returning `unnest` does not permute the settings *within* a role -- by confirming
the planner puts the sort below the `ProjectSet`.

### D8. `append_depends_on_extension()` does not order its rows

The query behind `ALTER ... DEPENDS ON EXTENSION` (`pg_dump.c:5702`) has no `ORDER BY`, so
an object with two extension dependencies emits them in `pg_depend` row order -- the order
the `ALTER ... DEPENDS ON EXTENSION` statements happened to run, and it changes if one is
dropped and re-added.  Affects every caller (`dumpFunc()`, `dumpTrigger()`, index and
materialized-view paths).  `ORDER BY 1` on the extension name is a complete key.

### D9. `collectSecLabels()` omits `provider` from its `ORDER BY`

```c
/* pg_dump.c:16755 */
"SELECT label, provider, classoid, objoid, objsubid "
"FROM pg_catalog.pg_seclabels ORDER BY classoid, objoid, objsubid"
```

`pg_seclabel`'s key is `(objoid, classoid, objsubid, provider)`.  Two providers labelling
one object produce two rows that tie under that `ORDER BY`, and the server's sort is not
stable, so the two `SECURITY LABEL FOR ...` statements come out in an order the catalog
does not determine.  `collectSecLabels()` is one of two sites; the shared-object path in `dumputils.c` has the
same gap.  Reaching it needs two registered label providers, which no in-tree module
supplied, so the committed test adds a second provider to
`src/test/modules/dummy_seclabel`, whose whole purpose is exercising this machinery.

### D10. `pg_dumpall`'s `dumpTablespaces()` orders by OID

```c
/* pg_dumpall.c:1368 */
"SELECT oid, spcname, ... FROM pg_catalog.pg_tablespace "
"WHERE spcname !~ '^pg_' "
"ORDER BY 1"          /* select-list column 1 is oid */
```

Select-list column 1 is `oid`, so the whole per-tablespace block -- `CREATE TABLESPACE`,
`ALTER TABLESPACE ... SET`, the ACL commands, `COMMENT`, `SECURITY LABEL` -- is emitted in
OID order.  That this is an off-by-one rather than intent is clear from the sibling
`dumpRoles()` at `pg_dumpall.c:855`, which has the identical select-list shape
(`SELECT oid, rolname, ...`) and says `ORDER BY 2`.  `spcname` alone is a complete key
(`pg_tablespace_spcname_index` is unique and `pg_tablespace` has no namespace).

### D11. `dumpExtension()` emits the `requires` array in dependency-array order

Under `--binary-upgrade`, `dumpExtension()` builds the seventh argument of
`binary_upgrade_create_empty_extension()` by walking `extinfo->dobj.dependencies[]` and
printing each `DO_EXTENSION` it finds (`pg_dump.c:11992`).  Nothing ever sorts a
`dependencies[]` array: `getDependencies()` ends `ORDER BY 1,2` -- `(classid, objid)`, with
`refobjid` absent -- so all of one extension's requires-rows tie and arrive in scan order,
which the backend wrote in *descending referenced-OID* order
(`eliminate_duplicate_dependencies()` &rarr; `object_address_comparator()`, "Primary sort
key is OID descending").  Two statements per database:

```sql
CREATE EXTENSION plperl;   CREATE EXTENSION hstore_plperl CASCADE;   -- ARRAY['hstore','plperl']
CREATE EXTENSION hstore;   CREATE EXTENSION hstore_plperl CASCADE;   -- ARRAY['plperl','hstore']
```

Only four in-tree control files list more than one `requires` entry
(`hstore_plperl`, `hstore_plperlu`, `hstore_plpython3u`, `ltree_plpython3u`), so the
reachable surface is narrow, but this is on the `pg_upgrade` path, which is where the
dump-comparison test lives.

---

## Considered and rejected

### Array-valued catalog columns reproduced verbatim -- *mostly* not a defect

This class was put to a dedicated adjudicator after the first workflow's judges split
three-to-one on it.  Its verdict: the two demonstrated cases are **not** defects, but the
sweep had stopped one array short, and that one **is** -- see D12 below.

#### The two demonstrated cases

Several catalog columns are arrays whose element order is an artifact of the order the DDL
was issued, and `pg_dump` reproduces that order.  The audit demonstrated it twice:

```sql
GRANT SELECT ON acl_t TO r_aaa;   GRANT SELECT ON acl_t TO r_bbb;
-- versus the same two GRANTs in the opposite order
```

```
=== A ===                                        === B ===
GRANT SELECT ON TABLE public.acl_t TO r_aaa;     GRANT SELECT ON TABLE public.acl_t TO r_bbb;
GRANT SELECT ON TABLE public.acl_t TO r_bbb;     GRANT SELECT ON TABLE public.acl_t TO r_aaa;
```

and the same for `pg_db_role_setting.setconfig` under `ALTER DATABASE ... SET`.  Four
agents split three-to-one on whether this belongs in the findings list.  It does not, for
three reasons, the third of which is decisive:

1. **It inverts the defect definition.**  The other findings are: identical catalog
   content, different OIDs, different output.  This is: *different* catalog content
   (`relacl` genuinely holds a different array value), identical OIDs, different output.
   `pg_dump` is reporting the catalog, not choosing an order.
2. **It is a fixed point.**  Dump, restore, dump again: the second dump equals the first.
   None of the harms that motivate this class occur -- no `Assert`, no tie, no
   `002_pg_upgrade.pl` mismatch.
3. **`buildACLCommands()`'s order is load-bearing.**  With a `WITH GRANT OPTION` chain, a
   grant must be replayed after the grant that authorised it.  A naive sort of the aclitem
   list would produce a dump that **fails to restore**.  Whatever is done here cannot be a
   plain sort.

The same argument covers every `*acl` column `pg_dump` feeds to `buildACLCommands()`
(`relacl`, `typacl`, `proacl`, `nspacl`, `defaclacl`, `lanacl`, `fdwacl`, `srvacl`,
`datacl`, `spcacl`, `lomacl`, parameter ACLs in `pg_dumpall`) and column-level ACLs, and it
covers `reloptions`, `proconfig` and `attoptions` for reason 1 alone.  Recorded here so it
is not re-proposed.

### Checked and found clean

* **The core regression database** (2291 relations) -- no ties, and byte-identical dump
  output across eight pre-sort shuffles.  Also clean under `--with-statistics`,
  `--no-owner`, `--no-privileges`, `--section=*`, `--schema-only`, `--data-only` and
  `--binary-upgrade`.
* **`TopoSort()` itself** -- given a fixed input order and a fixed dependency graph, its
  output is deterministic; the binary heap is keyed on the input index.  The instability in
  D2 is in what feeds it on failure, not in the sort.
* **The archive TOC** -- `-Fc` TOC order and single-threaded `pg_restore -f -` output
  follow the same sorted list as the plain dump.  (`pg_restore -j` deliberately does not,
  as the comment above `Assert(false)` already says.)
* **The rest of `pg_dumpall.c`** -- roles, role memberships, role GUC settings, databases
  and subscriptions are all ordered by name; `dumpTablespaces()` (D10) is the only one that
  is not.
* **`getDependencies()`'s `ORDER BY 1,2`** -- incomplete as a key, but the only place a
  `dependencies[]` array's order reaches the output is D11.
* **The comparator's helper functions** -- `pgTypeNameCompare()` compares
  `(nspname, typname)`, `accessMethodNameCompare()` compares `amname`; both are complete
  for their catalogs, and both handle the not-found case by returning "equal" so the caller
  falls through to its next basis for comparison.
* **Comments** -- `collectComments()` orders by `(classoid, objoid, objsubid)`, which is
  `pg_description`'s whole key; only the security-label sibling (D9) has a fourth key
  column.

---

## Tests and sample fixes on this branch

Eleven of the twelve findings have both a regression test and a sample fix.  D2 has
neither, for the reason given in Part 2.

| # | Test | Sample fix |
|---|---|---|
| D1 | `002_pg_dump.pl`, policy named after its own table | `pg_dump_sort.c`: compare `polname == NULL` after the table name |
| D3 | `002_pg_dump.pl`, `inh_order_child` | `pg_dump.c`: `ORDER BY inhrelid, inhseqno` |
| D4 | `002_pg_dump.pl`, `op_family` | `pg_dump.c`: add the member type names to all four member queries |
| D5 | `002_pg_dump.pl`, policy `p7` with a multi-role `TO` list | `pg_dump.c`: `unnest(polroles) WITH ORDINALITY` |
| D6 | `002_pg_dump.pl`, publications `pub9`/`pub10` | `pg_dump.c`: `ORDER BY n.nspname, c.relname` |
| D7 | `002_pg_dump.pl`, `ALTER ROLE ... IN DATABASE` | `pg_dump.c`: `ORDER BY rolname` |
| D8 | `test_pg_dump/t/001_base.pl` | `pg_dump.c`: `ORDER BY e.extname` |
| D9 | `003_pg_dump_with_server.pl` (+ a second provider in `dummy_seclabel`) | `pg_dump.c`, `dumputils.c`: add `provider` to both `ORDER BY`s |
| D10 | `002_pg_dump.pl`, `CREATE TABLESPACE in name order` | `pg_dumpall.c`: `ORDER BY 1` &rarr; `ORDER BY 2` |
| D11 | `003_pg_dump_with_server.pl` | `pg_dump.c`: sort the requires names with `pg_qsort_strcmp` |
| D12 | `002_pg_dump.pl`, `ALTER DEFAULT PRIVILEGES grantees ... in name order` | `pg_dump.c`: re-sort `defaclacl` by aclitem text under `COLLATE "C"` |

**The sample fixes are not proposed patches.**  They exist so the branch is coherent -- the
tests need something to pass against -- and so that "this test fails without the fix" is a
statement someone can check.  They are in their own commit and can be dropped wholesale.
Four of them involve a judgement a committer should make rather than accept:

* **D5** could instead be `ORDER BY rolname`.  The committed fix preserves the order the
  user wrote in `CREATE POLICY`, which round-trips; alphabetical order would be simpler but
  would rewrite the clause.  Both remove the OID dependence.
* **D4** orders by the members' type names.  Ordering by `regtype` output would have been
  shorter, but that rendering depends on `search_path`, so the fix joins `pg_type` and
  `pg_namespace` and orders by `(nspname, typname)` -- the same key
  `pgTypeNameCompare()` uses.
* **D1** sorts the RLS-enable pseudo-object *before* the policies on its table.  Either
  order is stable; this one matches `ENABLE ROW LEVEL SECURITY` logically preceding them.
* **D12** sorts an ACL array, which the sibling `relacl` case shows can be unsafe.  The
  argument that it is safe *here* -- a default ACL's items all share one grantor, so there
  is no grant chain to replay in order -- is the whole basis of the fix, and is the thing
  to check before accepting it.

Two findings needed test infrastructure rather than just a test entry.  D8 lives in
`src/test/modules/test_pg_dump` because showing it needs one object with **two** extension
dependencies, and a bare `initdb` has exactly one extension (`plpgsql`); `src/bin/pg_dump`'s
test install does not build contrib, so a test in `002_pg_dump.pl` would have to make the
core pg_dump suite depend on contrib.  `test_pg_dump` already installs its own extension
and already owns the only existing `DEPENDS ON EXTENSION` coverage.  D11 sidesteps the same
problem differently: its test writes three throwaway control files into the test's temp
directory and points `extension_control_path` at them, so it needs no contrib at all.

## Verification

Three runs of `meson test --suite setup --suite pg_dump --suite test_pg_dump --suite
dummy_seclabel`, on this branch, in this order.

**1. Everything applied: 13/13 pass**, including `002_pg_dump` with 13697 subtests.

**2. All five product files reverted, tests kept: 3 suites fail.**  `002_pg_dump` dies
early:

```
# pg_dump: ../ss-audit/src/bin/pg_dump/pg_dump_sort.c:511: DOTypeNameCompare: Assertion `0' failed.
#   Failed test 'binary_upgrade: pg_dump runs'
```

That is D1 doing what it should -- and it is also why this run alone is not enough: the
abort kills the dump before the ordering tests can be evaluated.

**3. Only D1's fix applied, the other ten reverted: 3 suites fail, each test by its own
name.**  `002_pg_dump` now runs to completion and fails on exactly the new entries:

```
should dump CREATE TABLE inh_order_child                          (D3)
should dump CREATE TABLE inh_order_child pg_upgrade               (D3)
should dump ALTER OPERATOR FAMILY dump_test.op_family USING btree (D4)
should dump CREATE POLICY p7 ON test_table with a multi-role TO list (D5)
should dump CREATE PUBLICATION pub9 / pub10                       (D6)
should dump ALTER ROLE ... IN DATABASE postgres SET, in role name order (D7)
should dump CREATE TABLESPACE in name order                       (D10)
should dump ALTER DEFAULT PRIVILEGES grantees are dumped in name order (D12)
```

`003_pg_dump_with_server` reports "failed 3 tests of 12" (D9 and D11), and
`test_pg_dump/001_base` fails (D8).  Every committed test fails for its own reason on the
unfixed tree.

Separately, each finding was re-checked outside the TAP suite by building the two databases
the report describes and diffing the dumps with the unfixed and the fixed binary.  All of
D4, D5, D6, D7, D8, D11 and D12 go from UNSTABLE to STABLE; D1 stops aborting; D3 emits
`INHERITS (public.p1, public.p2)` and restores the child with its columns in the original
order; D10 emits the tablespaces in name order.

**What is still unverified.**  The completeness critic that examined the D1 claim -- 48
object types, 55 construction sites, five SQL corpora each under 17 pg_dump option sets,
all 59 contrib extensions, plus the regression database -- returned "claim holds", and
named what it could not reach: cross-version dumps (pg_dump's older-server query branches),
`DO_SUBSCRIPTION_REL`, multi-encoding collations, and catalog corruption.  D2 is reported
without a fix or a test by choice.  Nothing else on this branch is unverified.

### D12. `getDefaultACLs()`: `defaclacl` is emitted in grantee-OID order

This one came out of the adjudication above, not out of discovery: the agent sent to settle
whether array order is ever a defect reproduced both demonstrated cases, agreed they are
not, and then checked the arrays the sweep had not.  `pg_default_acl.defaclacl` is a
different animal:

```sql
CREATE ROLE r_aaa;  CREATE ROLE r_bbb;                     -- database A
ALTER DEFAULT PRIVILEGES GRANT SELECT ON TABLES TO r_aaa;
ALTER DEFAULT PRIVILEGES GRANT SELECT ON TABLES TO r_bbb;
-- database B: identical, only the two CREATE ROLE lines swapped
```

```
D12-defaclacl/OLD: UNSTABLE
    -ALTER DEFAULT PRIVILEGES FOR ROLE postgres GRANT SELECT ON TABLES TO r_aaa;
    +ALTER DEFAULT PRIVILEGES FOR ROLE postgres GRANT SELECT ON TABLES TO r_aaa;
D12-defaclacl/NEW: STABLE (dumps identical)
```

The reason it is a defect where `relacl` is not: the backend **throws the DDL order away**.
`ExecGrant_Default_Acl()` canonicalizes the array with `aclitemsort()`, which orders by
grantee OID.  So the stored order is not "what the user wrote", it is a function of role
OIDs -- and a restore into a cluster that assigns different role OIDs produces a different
canonical order.  That also removes the objection that killed the `relacl` case: a default
ACL cannot contain a chain of grants by different grantors (every item's grantor is
`defaclrole`), so `buildACLCommands()`'s load-bearing replay order does not apply and
sorting is safe.

Sorting by the aclitem's text under `COLLATE "C"` in `getDefaultACLs()` fixes it.

The same adjudicator's negative results are worth as much as the finding, and are why the
`relacl` and `setconfig` cases stay rejected: it fuzzed 160 tables, 20 functions, 10 schemas
and 10 types with random `GRANT`/`REVOKE` histories, non-owner grantors, `PUBLIC`, column
privileges and three grant-option holders, then dumped, restored and re-dumped -- byte
identical.  It also ran a real `pg_upgrade` and confirmed that although the catalog arrays
*are* rewritten, `pg_dump` already normalizes around it (it drops items matching
`acldefault` and hoists owner self-grants into `firstsql`), so the dump comparison passes.
And it confirmed that `relacl` order really is load-bearing, by replaying a grant chain in
grantee-name order and getting `ERROR: permission denied for table t5`.

---

## Appendix A -- the instrumented build

Applied to `sortDumpableObjectsByTypeName()` in `pg_dump_sort.c` for the audit build only;
never committed.

```c
	/* PGDUMP_SHUFFLE_SEED=N: permute the array before sorting. */
	{
		const char *seedstr = getenv("PGDUMP_SHUFFLE_SEED");

		instr_tie_zero = (getenv("PGDUMP_TIE_ZERO") != NULL);
		if (seedstr != NULL && numObjs > 1)
		{
			srand((unsigned int) atoi(seedstr));
			for (int i = numObjs - 1; i > 0; i--)
			{
				int			j = rand() % (i + 1);
				DumpableObject *tmp = objs[i];

				objs[i] = objs[j];
				objs[j] = tmp;
			}
		}
	}

	if (numObjs > 1)
		qsort(objs, numObjs, sizeof(DumpableObject *), DOTypeNameCompare);

	/* PGDUMP_TIE_REPORT=1: report every adjacent pair that reached the
	 * comparator's fall-through.  Ties are adjacent after a sort, so this
	 * enumerates all of them. */
	if (getenv("PGDUMP_TIE_REPORT") != NULL)
	{
		for (int i = 1; i < numObjs; i++)
		{
			instr_tie_fallthrough = false;
			DOTypeNameCompare(&objs[i - 1], &objs[i]);
			if (instr_tie_fallthrough)
			{
				char		buf1[512], buf2[512];

				describeDumpableObject(objs[i - 1], buf1, sizeof(buf1));
				describeDumpableObject(objs[i], buf2, sizeof(buf2));
				fprintf(stderr, "SORT TIE: objType %d name \"%s\" nsp \"%s\" | %s | %s\n",
						(int) objs[i]->objType, objs[i]->name,
						objs[i]->namespace ? objs[i]->namespace->dobj.name : "(none)",
						buf1, buf2);
			}
		}
	}
```

and, in `DOTypeNameCompare()`, the fall-through becomes

```c
	instr_tie_fallthrough = true;
	if (instr_tie_zero)
		return 0;
	return oidcmp(obj1->catId.oid, obj2->catId.oid);
```

**The first version of this was wrong and reported nothing**, because disarming
`Assert(false)` left the fall-through returning `oidcmp()`, so the reporter's
"did these two compare equal?" test never fired.  It was caught only by running the
detector against a defect already known to be present.  A detector that silently finds
nothing is the failure mode that would have turned this report into "no defects", so
calibrate any replacement the same way.

## Appendix B -- minimal reproducers

Each is a complete `.sql` for a fresh database.  Where a finding is a two-database
comparison, both variants are given; dump each with the stated options and diff, after
normalizing pg_dump's random `\restrict` token
(`sed -E 's/^(\\(un)?restrict) [A-Za-z0-9]+$/\1 XXX/'`).

```sql
-- D1: assert-enabled pg_dump aborts.
CREATE TABLE pol_t (i int);
ALTER TABLE pol_t ENABLE ROW LEVEL SECURITY;
CREATE POLICY pol_t ON pol_t USING (true);

-- D2: two databases, only the two CREATE DOMAIN lines swapped.
CREATE DOMAIN d1 AS int;
CREATE DOMAIN d2 AS int;
ALTER DOMAIN d1 ADD CONSTRAINT c1 CHECK ((CAST(VALUE AS int)::d2) IS NOT NULL);
ALTER DOMAIN d2 ADD CONSTRAINT c2 CHECK ((CAST(VALUE AS int)::d1) IS NOT NULL);

-- D3: one database.  Dump, restore, and compare ch's column order.
CREATE TABLE p1 (a int);
CREATE TABLE p2 (b int);
CREATE TABLE decoy () INHERITS (p1);
CREATE TABLE ch (b int) INHERITS (p1);
DROP TABLE decoy;
VACUUM pg_inherits;
ALTER TABLE ch INHERIT p2;

-- D4: two databases, the two ADD FUNCTION lines swapped.
CREATE OPERATOR FAMILY myfam USING btree;
ALTER OPERATOR FAMILY myfam USING btree ADD FUNCTION 1 btint4cmp(int4, int4);
ALTER OPERATOR FAMILY myfam USING btree ADD FUNCTION 1 btint8cmp(int8, int8);

-- D5: two databases, the two CREATE ROLE lines swapped.
CREATE ROLE alice NOLOGIN;  CREATE ROLE bob NOLOGIN;
CREATE TABLE t (a int);
CREATE POLICY p ON t TO alice, bob USING (true);

-- D6: two databases, the EXCEPT list written in the opposite order.
CREATE TABLE ta (x int);  CREATE TABLE tb (x int);
CREATE PUBLICATION p FOR ALL TABLES EXCEPT (TABLE ta, TABLE tb);

-- D7: two databases, the two CREATE ROLE lines swapped.  Dump with --create.
CREATE ROLE ra NOLOGIN;  CREATE ROLE rb NOLOGIN;
ALTER ROLE ra IN DATABASE postgres SET work_mem='5MB';
ALTER ROLE rb IN DATABASE postgres SET work_mem='6MB';

-- D8: two databases, the two ALTER TRIGGER lines swapped.
CREATE EXTENSION cube;
CREATE TABLE t (a int);
CREATE TRIGGER tg BEFORE UPDATE ON t FOR EACH ROW
    EXECUTE FUNCTION suppress_redundant_updates_trigger();
ALTER TRIGGER tg ON t DEPENDS ON EXTENSION plpgsql;
ALTER TRIGGER tg ON t DEPENDS ON EXTENSION cube;

-- D9: needs two registered label providers; see the committed test, which adds a
-- second provider to src/test/modules/dummy_seclabel.

-- D10: two clusters, the two CREATE TABLESPACE lines swapped.  pg_dumpall --globals-only.
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE ts_aaa LOCATION '';
CREATE TABLESPACE ts_bbb LOCATION '';

-- D11: two databases.  Dump with --binary-upgrade.
CREATE EXTENSION plperl;   CREATE EXTENSION hstore_plperl CASCADE;   -- variant A
CREATE EXTENSION hstore;   CREATE EXTENSION hstore_plperl CASCADE;   -- variant B

-- D12: two databases, the two CREATE ROLE lines swapped.
CREATE ROLE r_aaa NOLOGIN;  CREATE ROLE r_bbb NOLOGIN;
ALTER DEFAULT PRIVILEGES GRANT SELECT ON TABLES TO r_aaa;
ALTER DEFAULT PRIVILEGES GRANT SELECT ON TABLES TO r_bbb;
```
