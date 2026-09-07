/* -------------------------------------------------------------------------
 *
 * rowsecurity.h
 *
 *	  prototypes for rewrite/rowsecurity.c and the structures for managing
 *	  the row security policies for relations in relcache.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#ifndef ROWSECURITY_H
#define ROWSECURITY_H

#include "nodes/parsenodes.h"
#include "utils/array.h"
#include "utils/relcache.h"

typedef struct RowSecurityPolicy
{
	char	   *policy_name;	/* Name of the policy */
	char		polcmd;			/* Type of command policy is for */
	ArrayType  *roles;			/* Array of roles policy is for */
	bool		permissive;		/* restrictive or permissive policy */
	Expr	   *qual;			/* Expression to filter rows */
	Expr	   *with_check_qual;	/* Expression to limit rows allowed */
	bool		hassublinks;	/* If either expression has sublinks */
} RowSecurityPolicy;

typedef struct RowSecurityDesc
{
	MemoryContext rscxt;		/* row security memory context */
	List	   *policies;		/* list of row security policies */
} RowSecurityDesc;

typedef List *(*row_security_policy_hook_type) (CmdType cmdtype,
												Relation relation);

extern PGDLLIMPORT row_security_policy_hook_type row_security_policy_hook_permissive;

extern PGDLLIMPORT row_security_policy_hook_type row_security_policy_hook_restrictive;

extern void get_row_security_policies(Query *root,
									  RangeTblEntry *rte, int rt_index,
									  List **securityQuals, List **withCheckOptions,
									  bool *hasRowSecurity, bool *hasSubLinks);

/*
 * Build the SELECT row-security quals for a single relation, independently of
 * any query/RTE, for use by the native graph executor.
 *
 * relid      : the (possibly leaf) relation that will actually be scanned
 * checkAsUser: user to evaluate policies/BYPASSRLS/ownership for (InvalidOid =
 *              GetUserId())
 * root_relid : ultimate inheritance root whose policies apply (== relid for a
 *              non-inherited relation; for a partition/inheritance child, its
 *              ancestor that carries the pg_policy rows)
 * target_varno: varno to which policy-qual Vars are mapped (policy quals are
 *              written against a single relation, varno 1; the caller usually
 *              passes OUTER_VAR)
 *
 * On return, *applies is true if RLS is active for relid (quals returned may
 * be a default-deny Const false).  *env_dep is set true when RLS is currently
 * bypassed but depends on the environment (role / row_security GUC), so the
 * plan must be invalidated on environment change.
 */
extern List *get_graph_row_security_quals(Oid relid, Oid checkAsUser,
										  Oid root_relid, Index target_varno,
										  bool *applies, bool *env_dep);

/*
 * Determine whether any element (backing) table of the given property graph
 * has (or may have) row-level security enabled for the current user.
 *
 * Scans pg_propgraph_element for the graph and calls check_enable_rls per
 * element table, returning true if any is RLS-enabled or environment-
 * dependent.  Shared by the rewriter (to set Query.hasRowSecurity so
 * plancache registers dependsOnRLS) and the native graph planner
 * (collect_graph_rls_info), so the RLS decision for a graph's elements is
 * single-sourced.
 */
extern bool graph_has_row_security(Oid graph_oid);

#endif							/* ROWSECURITY_H */
