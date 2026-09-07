/*-------------------------------------------------------------------------
 *
 * nodeGraphScan.c
 *	  routines to support native execution of property graph scans (SQL/PGQ)
 *
 * This module implements a lazy, hop-based Depth-First Search (DFS)
 * state machine for executing GRAPH_TABLE clauses.
 *
 * A query like (a)->(b)->(c) is decomposed into hops: (a)->(b) and (b)->(c).
 * Each hop is an iterator that yields (source_vertex, edge, dest_vertex)
 * triples.  Multi-hop is pipelined: for each output of hop 0, hop 1 runs
 * its full inner loop.  Backtracking is natural: when a hop exhausts its
 * edges, we backtrack to the previous hop to try its next destination.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeGraphScan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/relscan.h"
#include "utils/fmgroids.h"
#include "catalog/pg_propgraph_element.h"
#include "catalog/pg_propgraph_element_label.h"
#include "catalog/pg_propgraph_label_property.h"
#include "executor/executor.h"
#include "executor/nodeGraphScan.h"
#include "executor/nodeForeignscan.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "optimizer/cost.h"
#include "parser/parse_graphtable.h"
#include "executor/spi.h"
#include "catalog/pg_inherits.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

static TupleTableSlot *ExecGraphScan(PlanState *pstate);
static void init_start_vertex(GraphScanState * node);
static bool vle_find_next_edge(GraphScanState * node,
							   Oid src_elemid,
							   int src_nkeys, Datum *src_values,
							   bool *src_nulls,
							   int *start_table_idx, int *start_scan_pass,
							   int edge_elem_idx, EdgeDirection dir,
							   GraphHopFrame * hop,
							   Oid *dest_elemid,
							   int *dest_nkeys, Datum **dest_values,
							   bool **dest_nulls);
static void vle_push_frame(GraphHopFrame * hop, Oid elem_oid, int nkeys,
						   Datum *values, bool *nulls);
static void vle_pop_frame(GraphScanState * node, GraphHopFrame * hop);
static void vle_activate(GraphScanState * node, GraphHopFrame * hop,
						 GraphElementPattern *edge_elem);
static bool is_property_on_all_candidates(GraphScanState * node, int pi,
										  Oid propid);
static void hop_close_edge_scan(GraphHopFrame * hop);
static void hop_reset(GraphHopFrame * hop);
static int	find_vertex_for_edge(GraphScanState * node, Oid vertex_elemid);
static void init_column_mapping(GraphScanState * node, EState *estate);
static bool element_matches_label_expr(GraphScanState * node, int ei,
									   Node *labelexpr);
static void fill_one_slot_attr(GraphScanState * node, TupleTableSlot *slot,
							   int slot_att, int ci);
static AttrNumber find_attnum_by_name(TupleTableSlot *slot, const char *name);
static AttrNumber find_attnum_by_desc(TupleDesc desc, const char *name);
static void ExtractTargetVertexKeys(TupleTableSlot *edge_slot,
									GraphElementRelInfo * edge_info,
									EdgeDirection dir, int scan_pass,
									int nkeys, Datum *values, bool *nulls);
static TupleTableSlot *fetch_vertex_by_pk(GraphElementRelInfo * vinfo,
										  Datum *pk_values, bool *pk_nulls,
										  int npk, EState *estate);
static Datum resolve_graph_property(GraphScanState * node,
									Oid propid, TupleTableSlot *slot,
									bool *isnull, bool is_edge);
static bool evaluate_graph_where(GraphScanState * node);
static bool evaluate_element_where(GraphScanState * node, int elem_idx,
								   TupleTableSlot *elem_slot);
static void compile_graph_where(GraphScanState * scanstate);
static bool evaluate_element_rls(GraphScanState * node,
								 GraphElementRelInfo * info,
								 TupleTableSlot *slot);
static void compile_element_rls(GraphScanState * scanstate);

/*
 * Mutator context for converting GraphPropertyRef nodes used in WHERE
 * clauses into Vars of a per-query virtual outer tuple.
 *
 * fixed_elem_idx >= 0: all GraphPropertyRefs resolve to this pattern element
 * (element-level WHERE).  fixed_elem_idx < 0: referent resolved per
 * elvarname (graph-level WHERE).
 *
 * Qual-compile mode (graph_qual_mode) assigns each GraphPropertyRef a
 * distinct virtual attribute in the compiled clause's virtual tuple; the
 * source element + property per attribute is recorded in qual_refs[] for
 * runtime filling by property name.
 */
typedef struct ConvertPropRefContext
{
	GraphScanState *scanstate;
	int			fixed_elem_idx;
	bool		graph_qual_mode;
	int			nqual_atts;		/* running virtual attr count */
	GraphRefInfo *qual_refs;	/* array (size = #occurrences) */
}			ConvertPropRefContext;

/* Initialize a ConvertPropRefContext for a fresh conversion pass. */
static void
init_prop_ref_context(ConvertPropRefContext * context,
					  GraphScanState * scanstate, int fixed_elem_idx)
{
	context->scanstate = scanstate;
	context->fixed_elem_idx = fixed_elem_idx;
	context->graph_qual_mode = false;
	context->nqual_atts = 0;
	context->qual_refs = NULL;
}

static void compile_qual_clause(ConvertPropRefContext * context, Node *clause,
								ExprState **expr_out, int *natts_out,
								GraphRefInfo * *refs_out,
								TupleTableSlot **slot_out);
static TupleTableSlot *fill_qual_slot(GraphScanState * node,
									  TupleTableSlot *virt_slot,
									  int natts, const GraphRefInfo * refs,
									  int fixed_elem_idx,
									  TupleTableSlot *forced_elem_slot);
static void check_table_privilege(Oid table_oid);
static void check_column_privilege(Oid table_oid, AttrNumber attnum);
static void check_computed_property_column(GraphScanState * scanstate,
										   int ei, Oid propid);
static void check_graph_privileges(GraphScanState * scanstate);
static void validate_graph_property_refs(GraphScanState * scanstate,
										 EState *estate);
static bool label_expr_matches(const GraphElementRelInfo * info,
							   Node *labelexpr);
static bool elem_has_label(const GraphElementRelInfo * info, Oid labelid);
static bool element_has_property(GraphScanState * node, int ei, Oid propid,
								 const List *label_oids);
static Node *strip_collate_mutator(Node *node);
static AttrNumber find_prop_attnum(GraphScanState * node,
								   TupleTableSlot *slot, Oid propid);
static void store_start_vertex_identity(GraphScanState * node);
static bool check_samevar_edge_identity(GraphScanState * node, int edge_elem_idx,
										GraphHopFrame * hop);
static bool check_samevar_vertex_identity(GraphScanState * node, int dest_elem_idx,
										  GraphHopFrame * hop,
										  GraphElementRelInfo * dest_info);
static void convert_GraphPropertyRef_to_Var(Node **node_p, int elem_idx,
											GraphScanState * scanstate);

/* Scan abstraction for non-heap relation types */
static bool is_plain_relation(GraphElementRelInfo * info);
static bool is_partitioned_or_inherited(GraphElementRelInfo * info);
static void element_scan_start(GraphElementRelInfo * info, Snapshot snapshot);
static bool element_scan_next(GraphElementRelInfo * info, TupleTableSlot *slot);
static void element_scan_end(GraphElementRelInfo * info);
static TupleTableSlot *element_make_slot(GraphElementRelInfo * info);
static void copy_child_slot(TupleTableSlot *dst, TupleTableSlot *src);

/* For partitioned/inherited elements: expand partition children */
static void expand_partition_children(GraphElementRelInfo * info);

static FmgrInfo *build_col_eq_finfos(Oid table_oid, int n, AttrNumber *attnums);
static bool graph_eq_datum(FmgrInfo *eqfi, Datum a, bool anull,
						   Datum b, bool bnull);

/*
 * Helper function to avoid repeating same line (and make it clearer)
 */
static inline bool
edge_is_incoming(EdgeDirection dir, int scan_pass)
{
	return dir == GRAPH_DIR_INCOMING || (dir == GRAPH_DIR_UNDIRECTED && scan_pass == 1);
}

/*
 * Build per-column equality functions for the given attribute numbers of a
 * relation, using each column's type's default equality operator.  Returns
 * NULL when n == 0.  Allocated in the caller's context; kept for the
 * lifetime of the graph scan.
 */
static FmgrInfo *
build_col_eq_finfos(Oid table_oid, int n, AttrNumber *attnums)
{
	FmgrInfo   *finfos;
	int			i;

	if (n <= 0)
		return NULL;

	finfos = (FmgrInfo *) palloc(sizeof(FmgrInfo) * n);

	for (i = 0; i < n; i++)
	{
		TypeCacheEntry *typentry;
		Oid			atttype;

		atttype = get_atttype(table_oid, attnums[i]);
		typentry = lookup_type_cache(atttype, TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(typentry->eq_opr))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("no equality operator for type \"%s\"",
							format_type_be(atttype))));
		finfos[i] = typentry->eq_opr_finfo;
	}
	return finfos;
}

/*
 * Type-safe comparison of two column values using the prebuilt equality
 * function for that column.  NULLs never match at all (a NULL key/FK value
 * cannot connect any rows), preserving the original semantics; only two
 * non-NULL values are compared.
 */
static bool
graph_eq_datum(FmgrInfo *eqfi, Datum a, bool anull,
			   Datum b, bool bnull)
{
	if (anull || bnull)
		return false;
	return DatumGetBool(FunctionCall2Coll(eqfi, InvalidOid, a, b));
}

/*
 * Match the given slot's columns at attnums[] against the supplied values,
 * using the prebuilt per-column equality functions.  values[]/nulls[] must
 * have at least ncols entries.  NULLs never match (see graph_eq_datum).
 */
static bool
slot_matches_values(TupleTableSlot *slot, int ncols,
					const AttrNumber *attnums, FmgrInfo *eq_finfos,
					const Datum *values, const bool *nulls)
{
	for (int ki = 0; ki < ncols; ki++)
	{
		bool		isnull;
		Datum		col_val;

		col_val = slot_getattr(slot, attnums[ki], &isnull);
		if (!graph_eq_datum(&eq_finfos[ki], col_val, isnull,
							values[ki], nulls[ki]))
			return false;
	}
	return true;
}

/*
 * Initialize the first natts values of a virtual tuple slot to NULL.  Called
 * right after ExecClearTuple so that no stale values are left behind when
 * only some attributes are filled afterwards.
 */
static void
init_slot_values_null(TupleTableSlot *slot, int natts)
{
	for (int ai = 0; ai < natts; ai++)
	{
		slot->tts_values[ai] = (Datum) 0;
		slot->tts_isnull[ai] = true;
	}
}

/*
 * Allocate (replacing any existing) a parallel array pair of n Datum values
 * and n bool null flags.  If src_values/src_nulls are non-NULL their contents
 * are copied in; otherwise the arrays are left uninitialized for the caller
 * to fill.  Keeping the two arrays allocated and freed together removes the
 * "leak one of the pair" liability at every call site.
 */
static void
set_datum_pair(int n,
			   Datum **values, bool **nulls,
			   const Datum *src_values, const bool *src_nulls)
{
	if (*values)
		pfree(*values);
	if (*nulls)
		pfree(*nulls);
	*values = (Datum *) palloc(sizeof(Datum) * n);
	*nulls = (bool *) palloc(sizeof(bool) * n);
	if (src_values)
		memcpy(*values, src_values, sizeof(Datum) * n);
	if (src_nulls)
		memcpy(*nulls, src_nulls, sizeof(bool) * n);
}

/*
 * Release a datum/null-flag pair allocated by set_datum_pair, NULL-ing both.
 */
static void
free_datum_pair(Datum **values, bool **nulls)
{
	if (*values)
		pfree(*values);
	if (*nulls)
		pfree(*nulls);
	*values = NULL;
	*nulls = NULL;
}

/*
 * Helper: return the pattern element at the given index in the first
 * path pattern.
 */
static inline GraphElementPattern *
GetPatternElement(GraphScanState * node, int index)
{
	GraphScan  *scan = (GraphScan *) node->ss.ps.plan;
	List	   *path;

	if (scan->graph_pattern == NULL ||
		scan->graph_pattern->path_pattern_list == NIL)
		return NULL;

	path = linitial(scan->graph_pattern->path_pattern_list);
	return (GraphElementPattern *) list_nth(path, index);
}

/*
 * Scan abstraction helpers for non-heap relation types.
 */

static inline bool
is_plain_relation(GraphElementRelInfo * info)
{
	return (info->caps & GRAPHELEM_PLAIN) != 0;
}

static inline bool
is_partitioned_or_inherited(GraphElementRelInfo * info)
{
	return (info->caps & GRAPHELEM_PART) != 0;
}

/*
 * Start or restart a scan on the given element.  For plain relations and
 * partitioned/inherited tables, this initializes the child scan state.
 * For views and foreign tables, it resets the subplan/FDW scan.
 */
static void
element_scan_start(GraphElementRelInfo * info, Snapshot snapshot)
{
	switch (info->caps)
	{
		case GRAPHELEM_PART:
			/* Store snapshot for lazy child scan creation */
			info->scan.child_snapshot = snapshot;
			/* End any existing child scans, then reset to first child */
			for (int ci = 0; ci < info->scan.nchildren; ci++)
			{
				if (info->scan.child_scans[ci])
				{
					table_endscan(info->scan.child_scans[ci]);
					info->scan.child_scans[ci] = NULL;
				}
				if (info->scan.child_slots[ci])
					ExecClearTuple(info->scan.child_slots[ci]);
			}
			info->scan.current_child = 0;
			return;

		case GRAPHELEM_PLAIN:
			{
				/* Open (or restart) the direct heap scan on the element table */
				if (info->scan.scan)
				{
					table_endscan(info->scan.scan);
					info->scan.scan = NULL;
				}
				info->scan.scan = table_beginscan(info->heap_rel, snapshot,
												  0, NULL, SO_NONE);
				return;
			}

		case GRAPHELEM_VIEW:
			{
				char	   *viewname;
				char	   *spi_sql;
				int			ret;
				uint64		ntuples;
				HeapTuple  *tuples;
				TupleDesc	tdesc;

				/* Run SELECT * FROM the view via SPI (identifier-quoted) */
				viewname = get_rel_name(info->table_oid);
				spi_sql = psprintf("SELECT * FROM %s",
								   quote_identifier(viewname));
				ret = SPI_connect();
				if (ret < 0)
					elog(ERROR, "SPI_connect failed for view scan");
				ret = SPI_execute(spi_sql, true, 0);
				if (ret < 0)
				{
					SPI_finish();
					elog(ERROR, "SPI_execute failed for view scan: %s",
						 spi_sql);
				}
				ntuples = SPI_processed;
				tdesc = SPI_tuptable->tupdesc;

				/*
				 * Copy all tuples into element-local storage so they survive
				 * SPI_finish and don't conflict with other view scans.
				 */
				if (ntuples > 0)
				{
					MemoryContext oldcxt;
					TupleDesc	my_tdesc;

					/*
					 * SPI_execute runs in SPI's memory context.  Switch to
					 * executor context so allocations survive SPI_finish.
					 */
					oldcxt = MemoryContextSwitchTo(TopMemoryContext);
					my_tdesc = CreateTupleDescCopy(tdesc);
					info->scan.view_slot = MakeSingleTupleTableSlot(my_tdesc, &TTSOpsVirtual);
					tuples = (HeapTuple *) palloc(sizeof(HeapTuple) * ntuples);
					for (uint64 vi = 0; vi < ntuples; vi++)
						tuples[vi] = heap_copytuple(SPI_tuptable->vals[vi]);
					MemoryContextSwitchTo(oldcxt);
					info->scan.view_ntuples = ntuples;
					info->scan.view_current = 0;
					info->scan.view_subplan = (PlanState *) tuples;
				}
				else
				{
					info->scan.view_ntuples = 0;
					info->scan.view_current = 0;
					info->scan.view_subplan = NULL;
					info->scan.view_slot = NULL;
				}
				SPI_finish();
				return;
			}

		case GRAPHELEM_FDW:
			ExecReScanForeignScan(info->scan.fdw_scan_state);
			return;

		default:
			elog(ERROR, "unexpected graph element caps %#x in element_scan_start",
				 info->caps);
	}
}

/*
 * Get the next tuple from the element's scan.  Returns true with the slot
 * filled, or false if exhausted.  For partitioned/inherited tables, this
 * iterates across child relations.  For views and FDW, it calls ExecProcNode.
 *
 * For plain relations, the caller should use table_scan_getnextslot directly
 * (for performance — this path is hot).
 */
static bool
element_scan_next(GraphElementRelInfo * info, TupleTableSlot *slot)
{
	switch (info->caps)
	{
		case GRAPHELEM_PART:
			{
				while (info->scan.current_child < info->scan.nchildren)
				{
					int			ci = info->scan.current_child;

					/* Lazily create child scan if needed */
					if (info->scan.child_scans[ci] == NULL)
					{
						info->scan.child_scans[ci] =
							table_beginscan(info->scan.child_rels[ci],
											info->scan.child_snapshot,
											0, NULL, SO_NONE);
					}

					if (table_scan_getnextslot(info->scan.child_scans[ci],
											   ForwardScanDirection,
											   info->scan.child_slots[ci]))
					{
						copy_child_slot(slot, info->scan.child_slots[ci]);
						return true;
					}

					/* Child exhausted, clean up and move to next */
					if (info->scan.child_scans[ci])
					{
						table_endscan(info->scan.child_scans[ci]);
						info->scan.child_scans[ci] = NULL;
					}
					info->scan.current_child++;
				}
				ExecClearTuple(slot);
				return false;
			}

		case GRAPHELEM_VIEW:
			{
				if (info->scan.view_current < (int) info->scan.view_ntuples)
				{
					HeapTuple  *tuples = (HeapTuple *) info->scan.view_subplan;
					HeapTuple	tuple = tuples[info->scan.view_current];
					TupleDesc	tdesc = info->scan.view_slot->tts_tupleDescriptor;

					info->scan.view_current++;
					ExecClearTuple(slot);
					for (int ai = 0; ai < tdesc->natts &&
						 ai < slot->tts_tupleDescriptor->natts; ai++)
					{
						bool		isnull;

						slot->tts_values[ai] =
							heap_getattr(tuple, ai + 1, tdesc, &isnull);
						slot->tts_isnull[ai] = isnull;
					}
					ExecStoreVirtualTuple(slot);
					return true;
				}
				ExecClearTuple(slot);
				return false;
			}

		case GRAPHELEM_FDW:
			{
				TupleTableSlot *result = ExecProcNode((PlanState *) info->scan.fdw_scan_state);

				if (TupIsNull(result))
				{
					ExecClearTuple(slot);
					return false;
				}
				ExecCopySlot(slot, result);
				return true;
			}

		case GRAPHELEM_PLAIN:

			/*
			 * Direct heap scan owned by element_scan_start.  When exhausted
			 * the caller ends the scan and moves on.
			 */
			if (info->scan.scan == NULL)
				return false;
			return table_scan_getnextslot(info->scan.scan,
										  ForwardScanDirection, slot);

		default:
			elog(ERROR, "unexpected graph element caps %#x in element_scan_next",
				 info->caps);
			return false;
	}
}

/*
 * copy_child_slot
 *
 *		Copy the contents of a child tuple slot into the element slot,
 *		copying only the columns the two slots have in common.
 *
 *		For partitioned/inherited backing tables, child relations may carry
 *		additional columns that the parent (and therefore the element slot)
 *		does not have.  ExecCopySlot() insists that both slots have the same
 *		number of attributes, so we cannot use it here.  Instead, copy each
 *		attribute the parent slot has, nulling out the remaining positions,
 *		and store a virtual tuple.
 */
static void
copy_child_slot(TupleTableSlot *dst, TupleTableSlot *src)
{
	int			dst_natts = dst->tts_tupleDescriptor->natts;
	int			src_natts = src->tts_tupleDescriptor->natts;
	int			ncopy = Min(dst_natts, src_natts);
	int			i;

	ExecClearTuple(dst);
	for (i = 0; i < dst_natts; i++)
	{
		bool		isnull;

		dst->tts_values[i] = (i < ncopy) ?
			slot_getattr(src, i + 1, &isnull) : (Datum) 0;
		dst->tts_isnull[i] = (i >= ncopy) || isnull;
	}
	ExecStoreVirtualTuple(dst);
}

/*
 * End and clean up a scan on the given element.  For partitioned/inherited
 * tables, ends all child scans.
 */
static void
element_scan_end(GraphElementRelInfo * info)
{
	switch (info->caps)
	{
		case GRAPHELEM_PART:
			for (int ci = 0; ci < info->scan.nchildren; ci++)
			{
				if (info->scan.child_scans[ci])
				{
					table_endscan(info->scan.child_scans[ci]);
					info->scan.child_scans[ci] = NULL;
				}
				if (info->scan.child_slots[ci])
					ExecClearTuple(info->scan.child_slots[ci]);
			}
			info->scan.current_child = 0;
			return;

		case GRAPHELEM_VIEW:
			if (info->scan.view_ntuples > 0)
			{
				HeapTuple  *tuples = (HeapTuple *) info->scan.view_subplan;

				for (uint64 vi = 0; vi < info->scan.view_ntuples; vi++)
					heap_freetuple(tuples[vi]);
				pfree(tuples);
				info->scan.view_subplan = NULL;
			}
			if (info->scan.view_slot)
			{
				ExecDropSingleTupleTableSlot(info->scan.view_slot);
				info->scan.view_slot = NULL;
			}
			info->scan.view_current = 0;
			info->scan.view_ntuples = 0;
			return;

		case GRAPHELEM_FDW:
			/* FDW scan state is cleaned up in element_info_cleanup */
			return;

		case GRAPHELEM_PLAIN:
			if (info->scan.scan)
			{
				table_endscan(info->scan.scan);
				info->scan.scan = NULL;
			}
			return;

		default:
			elog(ERROR, "unexpected graph element caps %#x in element_scan_end",
				 info->caps);
	}
}

/*
 * Create a tuple table slot appropriate for the element type.
 * For plain relations and partitioned/inherited tables, uses
 * table_slot_callbacks.
 */
static TupleTableSlot *
element_make_slot(GraphElementRelInfo * info)
{
	switch (info->caps)
	{
		case GRAPHELEM_PLAIN:
			return MakeSingleTupleTableSlot(info->tupdesc,
											table_slot_callbacks(info->heap_rel));

		case GRAPHELEM_PART:

			/*
			 * Use virtual tuple ops so we can manually copy attributes from
			 * child slots without triggering ExecCopySlot's natts assertion.
			 */
			return MakeSingleTupleTableSlot(info->tupdesc, &TTSOpsVirtual);

		case GRAPHELEM_VIEW:
			/* Virtual slot — filled from SPI HeapTuple results */
			return MakeSingleTupleTableSlot(info->tupdesc, &TTSOpsVirtual);

		case GRAPHELEM_FDW:
			return MakeSingleTupleTableSlot(info->tupdesc,
											&TTSOpsHeapTuple);

		default:
			elog(ERROR, "unexpected graph element caps %#x in element_make_slot",
				 info->caps);
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * Expand a partitioned or inheritance-parent relation into its leaf children.
 * Called during build_element_info for elements with
 * relkind == RELKIND_PARTITIONED_TABLE or relkind == RELKIND_RELATION with
 * relhassubclass == true.
 */
static void
expand_partition_children(GraphElementRelInfo * info)
{
	List	   *inhoids;
	ListCell   *lc;
	int			nvalid = 0;
	int			maxchildren;

	/*
	 * find_all_inheritors returns the relation itself plus all descendants,
	 * regardless of whether they are partition children or traditional
	 * inheritance children (both handle nested hierarchies).  Filter to leaf
	 * relations: skip intermediate partitioned tables, which carry no rows of
	 * their own.
	 */
	inhoids = find_all_inheritors(info->table_oid,
								  AccessShareLock, NULL);
	maxchildren = list_length(inhoids);
	if (maxchildren <= 0)
	{
		info->scan.nchildren = 0;
		return;
	}

	info->scan.child_rels = (Relation *)
		palloc0(sizeof(Relation) * maxchildren);
	info->scan.child_scans = (struct TableScanDescData **)
		palloc0(sizeof(struct TableScanDescData *) * maxchildren);
	info->scan.child_slots = (TupleTableSlot **)
		palloc0(sizeof(TupleTableSlot *) * maxchildren);

	foreach(lc, inhoids)
	{
		Oid			child_oid = lfirst_oid(lc);
		Relation	child_rel;

		/* The relation itself is always first in the list */
		if (lc == list_head(inhoids))
		{
			/*
			 * For a traditional inheritance parent the parent itself can hold
			 * rows, so it is scanned as child[0] (reusing heap_rel). For a
			 * partitioned table the parent is never a leaf; skip it and scan
			 * only the leaves.
			 */
			if (info->relkind == RELKIND_RELATION)
			{
				child_rel = info->heap_rel;
				info->heap_rel = NULL;
			}
			else
				continue;
		}
		else
		{
			char		relkind = get_rel_relkind(child_oid);

			/* Skip intermediate partitioned tables (no rows of their own) */
			if (relkind == RELKIND_PARTITIONED_TABLE)
				continue;

			child_rel = try_table_open(child_oid, AccessShareLock);
			if (child_rel == NULL)
			{
				ereport(WARNING,
						(errmsg("skipping inheritance child with OID %u "
								"in native graph scan", child_oid),
						 errdetail("The relation does not exist.")));
				continue;
			}
		}

		info->scan.child_rels[nvalid] = child_rel;
		info->scan.child_slots[nvalid] =
			MakeSingleTupleTableSlot(RelationGetDescr(child_rel),
									 table_slot_callbacks(child_rel));
		nvalid++;
	}

	info->scan.nchildren = nvalid;
	info->scan.current_child = 0;
	list_free(inhoids);

	/* If no valid children remained after skipping, restore heap_rel */
	if (nvalid == 0)
	{
		if (info->scan.child_rels)
			pfree(info->scan.child_rels);
		if (info->scan.child_scans)
			pfree(info->scan.child_scans);
		if (info->scan.child_slots)
			pfree(info->scan.child_slots);
		info->scan.child_rels = NULL;
		info->scan.child_scans = NULL;
		info->scan.child_slots = NULL;
		info->heap_rel = try_table_open(info->table_oid, AccessShareLock);
		return;
	}

	/*
	 * If the parent (index 0) was skipped (e.g. non-existent OID), ensure
	 * child[0] is non-NULL so element_scan_* works.
	 */
	if (info->scan.child_rels[0] == NULL)
	{
		info->scan.child_rels[0] = try_table_open(info->table_oid,
												  AccessShareLock);
		if (info->scan.child_rels[0] != NULL)
		{
			info->scan.child_slots[0] =
				MakeSingleTupleTableSlot(RelationGetDescr(info->scan.child_rels[0]),
										 table_slot_callbacks(info->scan.child_rels[0]));
		}
	}
}

/*
 * Report an access-denied error for the named table, tolerating a missing
 * relation name.
 */
static void
report_acl_error(Oid table_oid)
{
	char	   *relname = get_rel_name(table_oid);

	aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE,
				   relname ? relname : "(unknown)");
}

/*
 * Check that the current user has SELECT privilege on the given table.
 * Called before accessing a table at runtime.  Only checks table-level
 * SELECT; column-level checks are done separately when resolving
 * specific properties.
 */
static void
check_table_privilege(Oid table_oid)
{
	if (pg_class_aclcheck(table_oid, GetUserId(),
						  ACL_SELECT) != ACLCHECK_OK)
		report_acl_error(table_oid);
}

/*
 * Check that the current user has column-level SELECT privilege on
 * the given column of a backing table.  Only call this when the user
 * does NOT have table-level SELECT on the table.
 */
static void
check_column_privilege(Oid table_oid, AttrNumber attnum)
{
	if (pg_attribute_aclcheck(table_oid, attnum, GetUserId(),
							  ACL_SELECT) != ACLCHECK_OK)
		report_acl_error(table_oid);
}

/*
 * At init time, for a computed property that maps a physical column
 * to a property alias (e.g. "three AS lttck"), look up the
 * pg_propgraph_label_property catalog to find the underlying column
 * reference and check column-level SELECT privilege.
 *
 * Only called when the user does NOT have table-level SELECT on the
 * backing table.  The element_oid identifies which element's label
 * the property belongs to.
 */
/*
 * Aliased computed property (e.g. "three AS lttck"): find the property
 * expression cached for this element and check column-level SELECT
 * privilege on the underlying column(s) it references.
 */
static void
check_computed_property_column(GraphScanState * scanstate, int ei,
							   Oid propid)
{
	GraphElementRelInfo *info = &scanstate->element_info[ei];

	for (GraphPropExpr * pe = info->prop_exprs;
		 pe != NULL; pe = pe->next)
	{
		if (pe->propid != propid || pe->expr == NULL)
			continue;
		if (IsA(pe->expr, Var))
		{
			Var		   *var = (Var *) pe->expr;

			check_column_privilege(info->table_oid, var->varattno);
		}
	}
}

/*
 * Check table-level and column-level SELECT privilege on all backing
 * tables that match pattern elements in the first path pattern.
 *
 * Runs at init time, before any row processing, so that LIMIT 0 and
 * other short-circuit optimizations don't bypass the privilege check.
 */
static void
check_graph_privileges(GraphScanState * scanstate)
{
	int			pi;

	for (pi = 0; pi < scanstate->num_pi; pi++)
	{
		GraphElementPattern *elem = GetPatternElement(scanstate, pi);
		int			ti;

		if (elem == NULL)
			continue;

		for (ti = 0; ti < scanstate->num_elements; ti++)
		{
			TupleDesc	desc;
			int			col;

			if (!element_matches_label_expr(scanstate, ti,
											elem->labelexpr))
				continue;

			/* Table-level SELECT granted, access to all columns */
			if (pg_class_aclcheck(
								  scanstate->element_info[ti].table_oid,
								  GetUserId(),
								  ACL_SELECT) == ACLCHECK_OK)
				continue;

			desc = scanstate->element_info[ti].tupdesc;

			/* Check column-level SELECT on all key columns */
			for (int ki = 0; ki < scanstate->element_info[ti].nkey_attnums; ki++)
			{
				check_column_privilege(
									   scanstate->element_info[ti].table_oid,
									   scanstate->element_info[ti].key_attnums[ki]);
			}

			/* Check column-level SELECT on each COLUMNS property */
			for (col = 0; col < scanstate->ncols; col++)
			{
				AttrNumber	attnum;

				if (scanstate->col_propname[col] == NULL)
					continue;
				if (scanstate->col_elem_index[col] != pi)
					continue;

				/* Try matching the property name as a physical column */
				attnum = find_attnum_by_desc(desc,
											 scanstate->col_propname[col]);

				if (attnum != InvalidAttrNumber)
					check_column_privilege(
										   scanstate->element_info[ti].table_oid, attnum);
				else if (scanstate->col_propid[col] != InvalidOid)
				{
					/*
					 * Aliased computed property (e.g. "three AS lttck"): look
					 * up the property expression cached for this element to
					 * find the underlying column reference.
					 */
					check_computed_property_column(scanstate, ti,
												   scanstate->col_propid[col]);
				}
			}
		}
	}
}

/*
 * Update the slot_for_elem cache so that fill_one_slot_attr can read
 * from the correct TupleTableSlot for each pattern element.
 *
 * Pattern element indices: 0=start_vertex, 1=edge0, 2=vertex0,
 * 3=edge1, 4=vertex1, ... etc.
 * For a vertex-only query (n_hops=0), only element 0 is valid.
 */
static void
update_slot_cache(GraphScanState * node)
{
	int			pi = 0;

	/* Element 0 is always the start vertex */
	if (node->start_vertex_slot)
		node->slot_for_elem[0] = node->start_vertex_slot;

	/* For each hop, elements are: edge (2*hi+1), dest vertex (2*hi+2) */
	for (int hi = 0; hi < node->n_hops; hi++)
	{
		GraphHopFrame *hop = &node->hops[hi];

		pi = 2 * hi + 1;
		if (pi < node->num_pi)
			node->slot_for_elem[pi] = hop->edge_slot;

		pi = 2 * hi + 2;
		if (pi < node->num_pi)
		{
			if (hop->to_vertex_slot)
				node->slot_for_elem[pi] = hop->to_vertex_slot;

			/*
			 * VLE zero-hop: no edge was traversed, so to_vertex_slot is not
			 * set.  The destination vertex is the same as the source.
			 */
			else if (hop->vle_active && hop->vle_stack_top == 0)
				node->slot_for_elem[pi] = hop->from_vertex_slot;
		}
	}
}

/*
 * Decode an int2[] catalog array (as returned by SysCacheGetAttr) into a
 * freshly-allocated array of AttrNumber.  On return *n holds the number of
 * elements; NULL (with *n = 0) is returned when the array is empty.
 */
static AttrNumber *
decode_int2_attnum_array(Datum arr, int *n)
{
	Datum	   *dvalues;
	int			nelems;
	AttrNumber *attnums;

	deconstruct_array_builtin(DatumGetArrayTypeP(arr), INT2OID,
							  &dvalues, NULL, &nelems);
	if (nelems == 0)
	{
		*n = 0;
		return NULL;
	}
	attnums = (AttrNumber *) palloc(sizeof(AttrNumber) * nelems);
	for (int ki = 0; ki < nelems; ki++)
		attnums[ki] = DatumGetInt16(dvalues[ki]);
	pfree(dvalues);
	*n = nelems;
	return attnums;
}

/*
 * Build the element info array from pg_propgraph_element.
 */
static void
build_element_info(GraphScanState * node)
{
	Relation	pg_rel;
	SysScanDesc scan;
	HeapTuple	tuple;
	int			count = 0;

	pg_rel = table_open(PropgraphElementRelationId, AccessShareLock);
	scan = systable_beginscan(pg_rel, InvalidOid, false, NULL, 0, NULL);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_propgraph_element elem = (Form_pg_propgraph_element) GETSTRUCT(tuple);

		if (elem->pgepgid == node->graph_oid)
			count++;
	}
	systable_endscan(scan);

	node->num_elements = count;
	if (count == 0)
	{
		node->element_info = NULL;
		table_close(pg_rel, AccessShareLock);
		return;
	}

	node->element_info = (GraphElementRelInfo *)
		palloc0(sizeof(GraphElementRelInfo) * count);

	scan = systable_beginscan(pg_rel, InvalidOid, false, NULL, 0, NULL);
	count = 0;
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_propgraph_element elem = (Form_pg_propgraph_element) GETSTRUCT(tuple);

		if (elem->pgepgid != node->graph_oid)
			continue;

		{
			HeapTuple	etup;
			bool		isnull;
			Datum		datum;

			etup = SearchSysCache1(PROPGRAPHELOID,
								   ObjectIdGetDatum(elem->oid));
			if (!HeapTupleIsValid(etup))
				elog(ERROR, "cache lookup failed for propgraph element %u",
					 elem->oid);

			node->element_info[count].element_oid = elem->oid;
			node->element_info[count].table_oid = elem->pgerelid;
			node->element_info[count].heap_rel =
				table_open(elem->pgerelid, AccessShareLock);
			{
				char		relkind = node->element_info[count].heap_rel->
					rd_rel->relkind;

				node->element_info[count].relkind = relkind;

				switch (relkind)
				{
					case RELKIND_RELATION:
						/* Check for traditional inheritance parent */
						if (node->element_info[count].heap_rel->rd_rel->
							relhassubclass)
						{
							/* Expand inheritance children */
							node->element_info[count].caps = GRAPHELEM_PART;
							expand_partition_children(
													  &node->element_info[count]);

							/*
							 * No valid children after expansion
							 * (expand_partition_children restored heap_rel in
							 * that case): fall back to a direct heap scan.
							 */
							if (node->element_info[count].scan.nchildren == 0)
								node->element_info[count].caps = GRAPHELEM_PLAIN;
							/* Use parent's tupdesc */
						}
						else
							node->element_info[count].caps = GRAPHELEM_PLAIN;
						break;

					case RELKIND_MATVIEW:
						/* Direct table scan — existing path */
						node->element_info[count].caps = GRAPHELEM_PLAIN;
						break;

					case RELKIND_PARTITIONED_TABLE:
						/* Expand into leaf partitions */
						node->element_info[count].caps = GRAPHELEM_PART;
						expand_partition_children(
												  &node->element_info[count]);
						break;

					case RELKIND_VIEW:

						/*
						 * View-backed element.  We scan it via SPI at
						 * element_scan_start/next time.  Just store the
						 * element info fields, the view's tupdesc comes from
						 * RelationGetDescr just like plain tables.
						 */
						node->element_info[count].caps = GRAPHELEM_VIEW;
						node->element_info[count].scan.view_subplan = NULL;
						node->element_info[count].scan.view_slot = NULL;
						break;

					default:
						{
							/*
							 * Close any relations already opened in this
							 * batch
							 */
							for (int ei = 0; ei < count; ei++)
							{
								if (node->element_info[ei].heap_rel)
								{
									table_close(node->element_info[ei].heap_rel,
												AccessShareLock);
									node->element_info[ei].heap_rel = NULL;
								}
							}
							systable_endscan(scan);
							table_close(pg_rel, AccessShareLock);
							ereport(ERROR,
									(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									 errmsg("cannot scan relation \"%s\" of type "
											"%c in native graph scan",
											RelationGetRelationName(
																	node->element_info[count].heap_rel),
											relkind),
									 errhint("Use SET enable_native_graphtable = off "
											 "for property graphs using views, "
											 "partitioned tables, or foreign "
											 "tables.")));
						}
				}
			}

			/*
			 * For expanded elements (inheritance/partitioned): heap_rel may
			 * be NULL after expand_partition_children moved it to
			 * child_rels[0].  Use child_rels[0] for the tupdesc.
			 */
			if (node->element_info[count].heap_rel)
				node->element_info[count].tupdesc =
					RelationGetDescr(node->element_info[count].heap_rel);
			else if (node->element_info[count].scan.nchildren > 0 &&
					 node->element_info[count].scan.child_rels)
				node->element_info[count].tupdesc =
					RelationGetDescr(node->element_info[count].scan.child_rels[0]);
			else
				node->element_info[count].tupdesc = NULL;
			node->element_info[count].element_kind = elem->pgekind;
			node->element_info[count].nkey_attnums = 0;
			node->element_info[count].key_attnums = NULL;
			node->element_info[count].nsrc_fk_attnums = 0;
			node->element_info[count].src_fk_attnums = NULL;
			node->element_info[count].ndst_fk_attnums = 0;
			node->element_info[count].dst_fk_attnums = NULL;

			/* Read vertex primary key array (pgekey) */
			datum = SysCacheGetAttr(PROPGRAPHELOID, etup,
									Anum_pg_propgraph_element_pgekey,
									&isnull);
			if (!isnull)
				node->element_info[count].key_attnums =
					decode_int2_attnum_array(datum,
											 &node->element_info[count].nkey_attnums);

			if (elem->pgekind != 'v')
			{
				/* Read source FK attnum array */
				node->element_info[count].src_vertex_elemid =
					elem->pgesrcvertexid;

				datum = SysCacheGetAttr(PROPGRAPHELOID, etup,
										Anum_pg_propgraph_element_pgesrckey,
										&isnull);
				if (!isnull)
					node->element_info[count].src_fk_attnums =
						decode_int2_attnum_array(datum,
												 &node->element_info[count].nsrc_fk_attnums);

				/* Read destination FK attnum array */
				node->element_info[count].dest_vertex_elemid =
					elem->pgedestvertexid;

				datum = SysCacheGetAttr(PROPGRAPHELOID, etup,
										Anum_pg_propgraph_element_pgedestkey,
										&isnull);
				if (!isnull)
					node->element_info[count].dst_fk_attnums =
						decode_int2_attnum_array(datum,
												 &node->element_info[count].ndst_fk_attnums);
			}

			ReleaseSysCache(etup);
		}
		count++;
	}
	systable_endscan(scan);
	table_close(pg_rel, AccessShareLock);

	/*
	 * Build the type-correct equality functions for each element's key and FK
	 * columns.  Several FK columns may reference the same vertex key columns;
	 * building them per backing table column lets every comparison at runtime
	 * use the right operator for the actual column type.
	 */
	for (int ei = 0; ei < node->num_elements; ei++)
	{
		GraphElementRelInfo *ei_info = &node->element_info[ei];
		Oid			table_oid = ei_info->table_oid;

		ei_info->key_eq = build_col_eq_finfos(table_oid,
											  ei_info->nkey_attnums,
											  ei_info->key_attnums);
		ei_info->src_fk_eq = build_col_eq_finfos(table_oid,
												 ei_info->nsrc_fk_attnums,
												 ei_info->src_fk_attnums);
		ei_info->dst_fk_eq = build_col_eq_finfos(table_oid,
												 ei_info->ndst_fk_attnums,
												 ei_info->dst_fk_attnums);
	}

	/* Build label membership for each element */
	for (int ei = 0; ei < node->num_elements; ei++)
	{
		Relation	el_rel;
		SysScanDesc el_scan;
		ScanKeyData key[1];
		HeapTuple	el_tup;

		ScanKeyInit(&key[0], Anum_pg_propgraph_element_label_pgelelid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(node->element_info[ei].element_oid));
		el_rel = table_open(PropgraphElementLabelRelationId,
							AccessShareLock);
		el_scan = systable_beginscan(el_rel,
									 PropgraphElementLabelElementLabelIndexId,
									 true, NULL, 1, key);
		while (HeapTupleIsValid(el_tup = systable_getnext(el_scan)))
		{
			Form_pg_propgraph_element_label el_form =
				(Form_pg_propgraph_element_label) GETSTRUCT(el_tup);
			int			nl = node->element_info[ei].nlabels;

			if (nl < lengthof(node->element_info[ei].label_oids))
				node->element_info[ei].label_oids[nl] = el_form->pgellabelid;
			else
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("too many labels (%d) on graph element",
								nl + 1)));

			node->element_info[ei].nlabels++;
		}
		systable_endscan(el_scan);
		table_close(el_rel, AccessShareLock);

		/*
		 * Populate the property-expression map for this element from
		 * pg_propgraph_label_property, keyed on the element-label binding row
		 * (identified by the element OID + label OID pair), mirroring the
		 * rewriter's lookup.
		 */
		for (int li = 0; li < node->element_info[ei].nlabels; li++)
		{
			Oid			label_oid = node->element_info[ei].label_oids[li];
			Oid			elem_labelid;
			Relation	pl_rel;
			SysScanDesc pl_scan;
			ScanKeyData pkey[1];
			HeapTuple	pl_tup;

			elem_labelid = GetSysCacheOid2(PROPGRAPHELEMENTLABELELEMENTLABEL,
										   Anum_pg_propgraph_element_label_oid,
										   ObjectIdGetDatum(node->element_info[ei].element_oid),
										   ObjectIdGetDatum(label_oid));
			if (!OidIsValid(elem_labelid))
				continue;

			ScanKeyInit(&pkey[0],
						Anum_pg_propgraph_label_property_plpellabelid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(elem_labelid));
			pl_rel = table_open(PropgraphLabelPropertyRelationId,
								AccessShareLock);
			pl_scan = systable_beginscan(pl_rel,
										 PropgraphLabelPropertyLabelPropIndexId,
										 true, NULL, 1, pkey);
			while (HeapTupleIsValid(pl_tup = systable_getnext(pl_scan)))
			{
				Form_pg_propgraph_label_property pl_form =
					(Form_pg_propgraph_label_property) GETSTRUCT(pl_tup);
				GraphPropExpr *pe;
				Datum		exprdatum;
				bool		exprnull;

				pe = palloc(sizeof(GraphPropExpr));
				pe->propid = pl_form->plppropid;
				pe->labelid = label_oid;
				exprdatum = heap_getattr(pl_tup,
										 Anum_pg_propgraph_label_property_plpexpr,
										 RelationGetDescr(pl_rel),
										 &exprnull);
				pe->expr = exprnull ? NULL :
					stringToNode(TextDatumGetCString(exprdatum));
				pe->next = node->element_info[ei].prop_exprs;
				node->element_info[ei].prop_exprs = pe;
			}
			systable_endscan(pl_scan);
			table_close(pl_rel, AccessShareLock);
		}
	}
}

/*
 * Compile each cached element property expression into an ExprState once at
 * scan init.  CollateExpr wrappers are stripped here (ExecInitExprRec cannot
 * evaluate them) rather than in the per-row path.  The compiled expression
 * reads its Var attributes from ecxt_scantuple, which resolve_graph_property
 * (and friends) bind to the element slot at evaluation time.
 *
 * The ExprState MUST be compiled with a NULL parent.  Passing the GraphScan's
 * PlanState would let ExecComputeSlotInfo() bind the EEOP_SCAN_FETCHSOME step
 * to the node's (fixed, virtual) scan tuple slot; at runtime ecxt_scantuple is
 * an element backing-table slot, so deforming it with the scan slot's ops can
 * read the wrong attribute (a NULL pointer with isnull=false), crashing
 * strict functions.  A NULL parent leaves the fetch step unfixed so the
 * element slot's own ops are used via slot_getsomeattrs(), which is exactly
 * how the previous per-row ExecInitExpr(..., NULL) behaved.
 */
static void
compile_property_exprs(GraphScanState * node)
{
	for (int ei = 0; ei < node->num_elements; ei++)
	{
		GraphElementRelInfo *info = &node->element_info[ei];

		for (GraphPropExpr * pe = info->prop_exprs;
			 pe != NULL; pe = pe->next)
		{
			Node	   *exprnode;

			if (pe->expr == NULL)
				continue;

			/* Strip CollateExpr wrappers (ExecInitExprRec can't handle them) */
			exprnode = strip_collate_mutator(pe->expr);
			pe->exprstate = ExecInitExpr((Expr *) exprnode,
										 (PlanState *) NULL);
		}
	}
}

/*
 * Build the per-element propid -> physical-column attnum cache.  For every
 * cached property binding we resolve the property expression's underlying
 * column in the backing table and record its attribute number, so the hot
 * per-row paths can read a property column directly without re-resolving the
 * property name and scanning the TupleDesc each time.
 *
 * Properties whose expression is not a plain Var(1, n) mapping to a physical
 * column (i.e. computed properties such as constants) are left out of the
 * cache; resolve_graph_property handles them via the compiled ExprState.
 */
static void
build_prop_attnum_cache(GraphScanState * node)
{
	for (int ei = 0; ei < node->num_elements; ei++)
	{
		GraphElementRelInfo *info = &node->element_info[ei];
		int			nphys = 0;

		/* Count physical-column (Var(1, attno)) bindings first */
		for (GraphPropExpr * pe = info->prop_exprs;
			 pe != NULL; pe = pe->next)
		{
			Var		   *var;

			if (pe->expr == NULL)
				continue;
			if (!IsA(pe->expr, Var))
				continue;
			var = (Var *) pe->expr;
			if (var->varno == 1 && var->varattno > 0)
				nphys++;
		}

		if (nphys == 0)
			continue;

		info->nprop_attnums = nphys;
		info->prop_attnums = (GraphPropAttnum *)
			palloc(sizeof(GraphPropAttnum) * nphys);

		nphys = 0;
		for (GraphPropExpr * pe = info->prop_exprs;
			 pe != NULL; pe = pe->next)
		{
			Var		   *var;

			if (pe->expr == NULL)
				continue;
			if (!IsA(pe->expr, Var))
				continue;
			var = (Var *) pe->expr;
			if (var->varno != 1 || var->varattno <= 0)
				continue;

			info->prop_attnums[nphys].propid = pe->propid;
			info->prop_attnums[nphys].attnum = var->varattno;
			nphys++;
		}
	}
}

/*
 * Find the backing-table attribute number of the given property for the
 * element owning slot, using the cache built at scan init.  Returns
 * InvalidAttrNumber when the property is not a physical column of the
 * element's backing table.
 */
static AttrNumber
find_prop_attnum(GraphScanState * node, TupleTableSlot *slot, Oid propid)
{
	int			ei;
	AttrNumber	result = InvalidAttrNumber;
	bool		found = false;

	if (slot == NULL || slot->tts_tupleDescriptor == NULL)
		return InvalidAttrNumber;

	/* Find which element OID this slot corresponds to */
	for (ei = 0; ei < node->num_elements && !found; ei++)
	{
		GraphElementRelInfo *info = &node->element_info[ei];

		if (!info->tupdesc)
			continue;
		if (info->tupdesc->tdtypeid != slot->tts_tupleDescriptor->tdtypeid)
			continue;

		for (int ai = 0; ai < info->nprop_attnums; ai++)
		{
			if (info->prop_attnums[ai].propid == propid)
			{
				result = info->prop_attnums[ai].attnum;
				found = true;
				break;
			}
		}
	}

	return result;
}

/*
 * Precompute, for every edge element, the element_info index of its source
 * and destination vertex elements.  These are looked up per matched edge at
 * runtime (see vle_find_next_edge); caching them avoids a linear scan of
 * element_info on the hot path.
 */
static void
precompute_vertex_indices(GraphScanState * node)
{
	node->src_vertex_idx = (int *) palloc(sizeof(int) * node->num_elements);
	node->dest_vertex_idx = (int *) palloc(sizeof(int) * node->num_elements);

	for (int ei = 0; ei < node->num_elements; ei++)
	{
		GraphElementRelInfo *info = &node->element_info[ei];

		if (info->element_kind == 'e' && info->src_vertex_elemid != InvalidOid)
			node->src_vertex_idx[ei] =
				find_vertex_for_edge(node, info->src_vertex_elemid);
		else
			node->src_vertex_idx[ei] = -1;

		if (info->element_kind == 'e' && info->dest_vertex_elemid != InvalidOid)
			node->dest_vertex_idx[ei] =
				find_vertex_for_edge(node, info->dest_vertex_elemid);
		else
			node->dest_vertex_idx[ei] = -1;
	}
}

static void
element_info_cleanup(GraphScanState * node)
{
	int			i;

	for (i = 0; i < node->num_elements; i++)
	{
		GraphElementRelInfo *ei = &node->element_info[i];

		switch (ei->relkind)
		{
			case RELKIND_RELATION:
			case RELKIND_MATVIEW:
				/* Check for inheritance parent with expanded children */
				if (ei->scan.nchildren > 0)
					goto cleanup_expanded;
				element_scan_end(ei);
				if (ei->heap_rel)
					table_close(ei->heap_rel, AccessShareLock);
				break;

			case RELKIND_PARTITIONED_TABLE:
		cleanup_expanded:
				element_scan_end(ei);
				for (int ci = 0; ci < ei->scan.nchildren; ci++)
				{
					if (ei->scan.child_rels && ei->scan.child_rels[ci])
						table_close(ei->scan.child_rels[ci], AccessShareLock);
					if (ei->scan.child_slots && ei->scan.child_slots[ci])
						ExecDropSingleTupleTableSlot(ei->scan.child_slots[ci]);
				}
				if (ei->scan.child_rels)
					pfree(ei->scan.child_rels);
				if (ei->scan.child_scans)
					pfree(ei->scan.child_scans);
				if (ei->scan.child_slots)
					pfree(ei->scan.child_slots);
				if (ei->heap_rel)
					table_close(ei->heap_rel, AccessShareLock);
				break;

			case RELKIND_VIEW:
				if (ei->scan.view_ntuples > 0)
				{
					HeapTuple  *tuples = (HeapTuple *) ei->scan.view_subplan;

					for (uint64 vi = 0; vi < ei->scan.view_ntuples; vi++)
						heap_freetuple(tuples[vi]);
					pfree(tuples);
				}
				if (ei->scan.view_slot)
					ExecDropSingleTupleTableSlot(ei->scan.view_slot);
				if (ei->heap_rel)
					table_close(ei->heap_rel, AccessShareLock);
				break;

			case RELKIND_FOREIGN_TABLE:
				if (ei->scan.fdw_scan_state)
					ExecEndForeignScan(ei->scan.fdw_scan_state);
				if (ei->heap_rel)
					table_close(ei->heap_rel, AccessShareLock);
				break;

			default:
				/* Unknown — try to close heap_rel anyway */
				if (ei->heap_rel)
					table_close(ei->heap_rel, AccessShareLock);
				break;
		}
	}
}

/*
 * Collect the indices of the element models matching a pattern element:
 * same kind (vertex vs edge) and a matching label expression.
 */
static void
collect_candidate_elements(GraphScanState * node, GraphElementPattern *gep,
						   List **candidates)
{
	for (int ei = 0; ei < node->num_elements; ei++)
	{
		GraphElementRelInfo *info = &node->element_info[ei];

		if (IS_EDGE_PATTERN(gep->kind))
		{
			if (info->element_kind != 'e')
				continue;
		}
		else if (info->element_kind != 'v')
			continue;

		if (!label_expr_matches(info, gep->labelexpr))
			continue;

		*candidates = lappend_int(*candidates, ei);
	}
}

/*
 * Return the table_index-th matching element info for a given pattern
 * element, considering both element kind and label expression.
 */
static GraphElementRelInfo *
GetElementRelInfo(GraphScanState * node, GraphElementPattern *elem,
				  int table_index)
{
	List	   *candidates = NIL;

	if (elem == NULL)
		return NULL;

	collect_candidate_elements(node, elem, &candidates);
	if (table_index < list_length(candidates))
		return &node->element_info[list_nth_int(candidates, table_index)];
	return NULL;
}

/* Forward declarations for property validation helpers */
static void walk_property_refs(Node *node, const char *varname, List **refs);

/*
 * Context and generic walker used for all GraphPropertyRef/Var scans in
 * this file.  It is built on expression_tree_walker and replaces the
 * previous hand-rolled per-purpose walkers (walk_property_refs,
 * find_ref_elem_index, find_varattno_in_expr).
 *
 * Depending on which fields are set, a single pass can:
 *   - collect GraphPropertyRef nodes (optionally filtered by varname) into
 *     *refs;
 *   - find the pattern element index referenced by the first matching
 *     GraphPropertyRef (found_elem_idx);
 *   - find the varattno of the first Var matching varno (found_varattno).
 *
 * If first_match is set, the walk stops as soon as the requested result has
 * been found and graph_expr_scanner returns true.
 */
typedef struct GraphExprScanContext
{
	const char *varname;		/* filter GraphPropertyRef by elvarname, or
								 * NULL */
	List	  **refs;			/* collect GraphPropertyRef nodes here */
	Index		varno;			/* Var varno to search */
	GraphScanState *scanstate;	/* needed to resolve elvarname -> elem index */
	int			found_elem_idx; /* result: pattern element index */
	int			found_varattno; /* result: attnum of matching Var */
	bool		first_match;	/* stop after first match */
}			GraphExprScanContext;

static bool
graph_expr_scanner(Node *node, void *context)
{
	GraphExprScanContext *ctx = (GraphExprScanContext *) context;

	if (node == NULL)
		return false;

	if (IsA(node, GraphPropertyRef))
	{
		GraphPropertyRef *gpr = (GraphPropertyRef *) node;

		if (ctx->refs != NULL &&
			(ctx->varname == NULL ||
			 (gpr->elvarname != NULL &&
			  strcmp(gpr->elvarname, ctx->varname) == 0)))
			*ctx->refs = lappend(*ctx->refs, gpr);

		if (ctx->scanstate != NULL && gpr->elvarname != NULL)
		{
			GraphScan  *plan = (GraphScan *) ctx->scanstate->ss.ps.plan;
			List	   *path =
				linitial(plan->graph_pattern->path_pattern_list);
			int			pi;

			for (pi = 0; pi < list_length(path); pi++)
			{
				GraphElementPattern *gep =
					(GraphElementPattern *) list_nth(path, pi);

				if (gep->variable != NULL &&
					strcmp(gep->variable, gpr->elvarname) == 0)
				{
					ctx->found_elem_idx = pi;
					if (ctx->first_match)
						return true;
					break;
				}
			}
		}
		return false;
	}

	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == ctx->varno)
		{
			ctx->found_varattno = var->varattno;
			if (ctx->first_match)
				return true;
		}
		return false;
	}

	return expression_tree_walker(node, graph_expr_scanner, context);
}

/*
 * Collect all GraphPropertyRef nodes matching varname in an expression
 * tree.  If varname is NULL nothing is collected: callers pass the pattern
 * element's variable name, which may be NULL for anonymous elements, and in
 * that case the element cannot reference any property by name.
 */
static void
walk_property_refs(Node *node, const char *varname, List **refs)
{
	GraphExprScanContext ctx;

	if (varname == NULL)
		return;

	ctx.varname = varname;
	ctx.refs = refs;
	ctx.varno = 0;
	ctx.scanstate = NULL;
	ctx.found_elem_idx = -1;
	ctx.found_varattno = -1;
	ctx.first_match = false;

	(void) graph_expr_scanner(node, &ctx);
}

/*
 * Find the pattern element index referenced by the first GraphPropertyRef
 * in an expression tree.  Returns -1 if none found.
 */
static int
find_ref_elem_index(Node *node, GraphScanState * scanstate)
{
	GraphExprScanContext ctx;

	ctx.varname = NULL;
	ctx.refs = NULL;
	ctx.varno = 0;
	ctx.scanstate = scanstate;
	ctx.found_elem_idx = -1;
	ctx.found_varattno = -1;
	ctx.first_match = true;

	(void) graph_expr_scanner(node, &ctx);
	return ctx.found_elem_idx;
}

/*
 * Find the varattno of the first Var referencing varno in an expression
 * tree.  Returns -1 if none found.
 */
static int
find_varattno_in_expr(Node *node, Index varno)
{
	GraphExprScanContext ctx;

	ctx.varname = NULL;
	ctx.refs = NULL;
	ctx.varno = varno;
	ctx.scanstate = NULL;
	ctx.found_elem_idx = -1;
	ctx.found_varattno = -1;
	ctx.first_match = true;

	(void) graph_expr_scanner(node, &ctx);
	return ctx.found_varattno;
}

/*
 * Unwrap expression wrapper nodes (RelabelType, CoerceViaIO, CollateExpr)
 * that do not change the underlying value, until a non-wrapper node is
 * reached.  FuncExpr is intentionally left in place: call sites decide
 * whether to descend into a function call.
 */
static Node *
strip_expr_wrappers(Node *node)
{
	for (;;)
	{
		if (IsA(node, RelabelType))
			node = (Node *) ((RelabelType *) node)->arg;
		else if (IsA(node, CoerceViaIO))
			node = (Node *) ((CoerceViaIO *) node)->arg;
		else if (IsA(node, CollateExpr))
			node = (Node *) ((CollateExpr *) node)->arg;
		else
			break;
	}
	return node;
}

/*
 * Executor-init validation: verify that every GraphPropertyRef in the
 * scan matches a property defined on at least one candidate backing
 * table.  Runs at executor init time so it catches schema changes
 * between PREPARE and EXECUTE.  Uses the element model built by
 * build_element_info, without re-scanning the catalog.
 */
static void
validate_graph_property_refs(GraphScanState * scanstate, EState *estate)
{
	GraphScan  *scan = (GraphScan *) scanstate->ss.ps.plan;

	if (scan->graph_pattern == NULL ||
		scan->graph_pattern->path_pattern_list == NIL)
		return;

	{
		List	   *path = linitial(scan->graph_pattern->path_pattern_list);
		int			pi;
		int			path_len;

		if (path == NULL)
			return;
		path_len = list_length(path);

		for (pi = 0; pi < path_len; pi++)
		{
			GraphElementPattern *gep = (GraphElementPattern *)
				list_nth(path, pi);
			List	   *label_oids = NIL;
			List	   *candidates = NIL;
			ListCell   *clc;

			if (gep == NULL)
				continue;

			if (gep->labelexpr != NULL)
			{
				label_oids = get_label_oids_for_labelexpr(gep->labelexpr);
			}

			/* Candidate elements: matching kind + matching label expression */
			collect_candidate_elements(scanstate, gep, &candidates);

			if (candidates == NIL)
				continue;

			{
				List	   *prop_refs = NIL;
				ListCell   *prlc;

				if (scan->graph_table_columns)
				{
					ListCell   *lc;

					foreach(lc, scan->graph_table_columns)
					{
						TargetEntry *gte = lfirst_node(TargetEntry, lc);
						Node	   *gcol = (Node *) gte->expr;

						while (IsA(gcol, FuncExpr))
							gcol = (Node *) linitial(((FuncExpr *) gcol)->args);
						gcol = strip_expr_wrappers(gcol);

						if (IsA(gcol, GraphPropertyRef))
						{
							GraphPropertyRef *gpr =
								(GraphPropertyRef *) gcol;

							if (gep->variable && gpr->elvarname &&
								strcmp(gep->variable,
									   gpr->elvarname) == 0)
								prop_refs = lappend(prop_refs, gpr);
						}
					}
				}

				if (gep->whereClause)
					walk_property_refs(gep->whereClause,
									   gep->variable, &prop_refs);
				if (gep->subexpr)
				{
					ListCell   *lc;

					foreach(lc, gep->subexpr)
						walk_property_refs((Node *) lfirst(lc),
										   gep->variable, &prop_refs);
				}

				if (scan->graph_pattern->whereClause)
					walk_property_refs(
									   scan->graph_pattern->whereClause,
									   gep->variable, &prop_refs);

				foreach(prlc, prop_refs)
				{
					GraphPropertyRef *gpr =
						(GraphPropertyRef *) lfirst(prlc);
					bool		found_in_any = false;

					foreach(clc, candidates)
					{
						if (element_has_property(scanstate,
												 lfirst_int(clc),
												 gpr->propid,
												 label_oids))
						{
							found_in_any = true;
							break;
						}
					}

					if (!found_in_any)
					{
						const char *propname =
							get_propgraph_property_name(gpr->propid);

						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("property \"%s\" for "
										"element variable \"%s\" "
										"not found",
										propname, gep->variable)));
					}
				}
			}
		}
	}
}

/*
 * Shared label-expression matching.
 *
 * A label expression is either a single GraphLabelRef or a BoolExpr (OR)
 * of GraphLabelRef nodes.  label_expr_matches() tests whether an element
 * (whose label membership is described by *info) has any of the referenced
 * labels.  The OR / GraphLabelRef traversal lives in the shared
 * graph_label_expr_matches() (parse_graphtable.c); this is the executor's
 * membership callback backed by the cached label_oids[].
 */
static bool
elem_has_label(const GraphElementRelInfo * info, Oid labelid)
{
	for (int li = 0; li < info->nlabels; li++)
		if (info->label_oids[li] == labelid)
			return true;
	return false;
}

static bool
graph_elem_has_label_member(Oid labelid, void *arg)
{
	const		GraphElementRelInfo *info = (const GraphElementRelInfo *) arg;

	return elem_has_label(info, labelid);
}

static bool
label_expr_matches(const GraphElementRelInfo * info, Node *labelexpr)
{
	return graph_label_expr_matches(labelexpr, graph_elem_has_label_member,
									(void *) info);
}

/*
 * Does element ei carry the given property under one of the labels in
 * label_oids (NIL means "any label")?  Uses the model built at scan init.
 */
static bool
element_has_property(GraphScanState * node, int ei, Oid propid,
					 const List *label_oids)
{
	for (GraphPropExpr * pe = node->element_info[ei].prop_exprs;
		 pe != NULL; pe = pe->next)
	{
		if (pe->propid != propid)
			continue;
		if (label_oids == NIL ||
			list_member_oid((List *) label_oids, pe->labelid))
			return true;
	}
	return false;
}


/* ----------------------------------------------------------------
 *	 ExecInitGraphScan
 * ----------------------------------------------------------------
 */
GraphScanState *
ExecInitGraphScan(GraphScan * node, EState *estate, int eflags)
{
	GraphScanState *scanstate;
	MemoryContext oldcxt;
	int			nelems;
	int			i;
	List	   *path_elements;

	scanstate = makeNode(GraphScanState);

	/*
	 * The native graph executor cannot run in a parallel worker: GraphPath
	 * never sets parallel_aware, and the execParallel.c machinery treats
	 * GraphScanState as an ordinary, serial scan.
	 */
	Assert(!node->scan.plan.parallel_aware);

	/*
	 * Per-query memory context owning all arrays/state allocated for this
	 * node.  Created first so every per-query palloc below can be switched
	 * into it; it is deleted once in ExecEndGraphScan.
	 */
	scanstate->graph_cxt = AllocSetContextCreate(CurrentMemoryContext,
												 "GraphScan",
												 ALLOCSET_DEFAULT_SIZES);

	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;
	scanstate->ss.ps.ExecProcNode = ExecGraphScan;

	scanstate->graph_oid = node->graph_oid;

	scanstate->start_vertex_slot = NULL;
	scanstate->n_start_keys = 0;
	scanstate->start_vertex_values = NULL;
	scanstate->start_vertex_nulls = NULL;
	scanstate->start_table_index = 0;
	scanstate->hops = NULL;
	scanstate->n_hops = 0;
	scanstate->current_hop = -1;
	scanstate->is_initialized = false;
	scanstate->path_len = 0;

	/* Count pattern elements from the first path pattern */
	scanstate->num_pi = 1;
	if (node->graph_pattern != NULL)
	{
		List	   *ppl = node->graph_pattern->path_pattern_list;

		if (ppl != NIL)
			scanstate->num_pi =
				list_length(linitial(ppl));
	}

	/* Compute number of hops: (N_pattern_elems - 1) / 2 */
	{
		int			pi = scanstate->num_pi;

		if (pi >= 3)
			scanstate->n_hops = (pi - 1) / 2;
		else
			scanstate->n_hops = 0;
	}

	/* Initialize expression context and result tuple slot */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * Share the executor's param values so that Param(PARAM_EXEC) and
	 * Param(PARAM_EXTERN) nodes in element WHERE clauses can be resolved
	 * (e.g., when a plpgsql function passes parameters to a graph query
	 * within it).
	 */
	scanstate->ss.ps.ps_ExprContext->ecxt_param_exec_vals =
		estate->es_param_exec_vals;
	scanstate->ss.ps.ps_ExprContext->ecxt_param_list_info =
		estate->es_param_list_info;

	ExecInitResultTupleSlotTL(&scanstate->ss.ps, &TTSOpsVirtual);

	/* Initialize qual */
	scanstate->ss.ps.qual = ExecInitQual(node->scan.plan.qual,
										 (PlanState *) scanstate);

	/*
	 * Create scan tuple slot with a TupleDesc matching only the graph columns
	 * (graph_table_columns), not the full plan targetlist. The scan slot is
	 * used when projection is active: Var references with varno=scanrelid
	 * read from this slot, while Const entries are evaluated directly by
	 * ExecProject.
	 */
	{
		List	   *gtcols = node->graph_table_columns;
		TupleDesc	scantupdesc;

		scantupdesc = CreateTemplateTupleDesc(list_length(gtcols));
		{
			ListCell   *lc;
			int			ci = 0;

			foreach(lc, gtcols)
			{
				TargetEntry *te = (TargetEntry *) lfirst(lc);

				TupleDescInitEntry(scantupdesc, (AttrNumber) (ci + 1),
								   NULL,
								   exprType((Node *) te->expr),
								   exprTypmod((Node *) te->expr),
								   0);
				ci++;
			}
		}

		ExecInitScanTupleSlot(estate, &scanstate->ss,
							  scantupdesc, &TTSOpsVirtual, 0);
	}

	/*
	 * Use ExecConditionalAssignProjectionInfo which checks if the target list
	 * matches the tuple descriptor.  Pass the scan slot's TupleDesc as
	 * inputDesc so that the projection correctly maps Var(varno=scanrelid)
	 * references to the scan slot's attributes.
	 */
	ExecAssignProjectionInfo(&scanstate->ss.ps,
							 scanstate->ss.ss_ScanTupleSlot->
							 tts_tupleDescriptor);

	/* Build element info array.  PG_TRY ensures clean state on error. */
	{
		PG_TRY();
		{
			build_element_info(scanstate);
		}
		PG_CATCH();
		{
			element_info_cleanup(scanstate);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	/*
	 * All per-query allocations below (column mapping, compiled quals,
	 * same-variable tracking, hop frames) are owned by graph_cxt and freed
	 * wholesale by MemoryContextDelete in ExecEndGraphScan.
	 */
	oldcxt = MemoryContextSwitchTo(scanstate->graph_cxt);

	/* Build column mapping for projection */
	init_column_mapping(scanstate, estate);

	/* Compile per-element RLS quals (from plan-time collection) */
	compile_element_rls(scanstate);

	/*
	 * Compile every cached element property expression once so the per-row
	 * property resolution path never recompiles them.
	 */
	compile_property_exprs(scanstate);

	/*
	 * Build the per-element propid -> physical-column attnum cache used by
	 * the per-row property read paths.
	 */
	build_prop_attnum_cache(scanstate);

	/*
	 * Precompute the element_info index of each edge's source/destination
	 * vertex element for the per-matched-edge hot path.
	 */
	precompute_vertex_indices(scanstate);

	/*
	 * Compile column expressions from graph_table_columns.  Every non-bare
	 * column (constants, function calls wrapping property references, etc.)
	 * is compiled to an ExprState here; bare GraphPropertyRef columns are
	 * handled by fill_one_slot_attr via runtime by-name resolution (safe
	 * across differing backing-table layouts).
	 */
	{
		List	   *gtcols = node->graph_table_columns;
		int			gtcol_count = list_length(gtcols);
		int			ci;

		scanstate->col_exprs = (ExprState **)
			palloc0(sizeof(ExprState *) * scanstate->ncols);
		scanstate->col_expr_elem_index = (int *)
			palloc0(sizeof(int) * scanstate->ncols);

		for (ci = 0; ci < scanstate->ncols; ci++)
		{
			scanstate->col_expr_elem_index[ci] = -1;

			if (ci < gtcol_count)
			{
				TargetEntry *gte = (TargetEntry *)
					list_nth(gtcols, ci);
				Node	   *gcol = (Node *) gte->expr;

				/*
				 * Determine which pattern element this column references.
				 * Prefer the index recorded by init_column_mapping.
				 */
				scanstate->col_expr_elem_index[ci] =
					scanstate->col_elem_index[ci];
				if (scanstate->col_expr_elem_index[ci] < 0)
					scanstate->col_expr_elem_index[ci] =
						find_ref_elem_index(gcol, scanstate);

				if (IsA(gcol, GraphPropertyRef))
				{
					/*
					 * Bare property column: resolved by name at runtime in
					 * fill_one_slot_attr (geometry-safe).  No ExprState.
					 */
					continue;
				}

				/*
				 * Bare Var referencing a real (non-scan) relation is a
				 * LATERAL outer reference resolved by the join machinery
				 * above us; do not compile it as a scan Var inside the node.
				 */
				if (IsA(gcol, Var) && ((Var *) gcol)->varno > 0)
					continue;

				if (scanstate->col_expr_elem_index[ci] >= 0)
					convert_GraphPropertyRef_to_Var(
													&gcol, scanstate->col_expr_elem_index[ci],
													scanstate);

				scanstate->col_exprs[ci] =
					ExecInitExpr((Expr *) gcol, (PlanState *) scanstate);
			}
		}
	}

	/* Check all graph privileges at init time (before LIMIT 0 can skip us) */
	check_graph_privileges(scanstate);

	/*
	 * Validate all property references against the backing tables. Run at
	 * executor init time (not plan time) so that schema changes between
	 * PREPARE and EXECUTE are caught.
	 */
	validate_graph_property_refs(scanstate, estate);

	/*
	 * Validate that every explicitly specified label in the pattern is
	 * associated with at least one element of the required kind.  Run at
	 * executor init time so that DDL performed after parsing (e.g. dropping a
	 * label from all vertex tables) is reflected.
	 */
	validate_graph_element_label_kinds(node->graph_pattern, node->graph_oid);

	/* Allocate slot_for_elem cache */
	scanstate->slot_for_elem = (TupleTableSlot **)
		palloc0(sizeof(TupleTableSlot *) * scanstate->num_pi);

	/* Compile element WHERE clauses into ExprStates */
	{
		path_elements = linitial(
								 node->graph_pattern->path_pattern_list);

		nelems = list_length(path_elements);
		scanstate->element_quals = (ExprState **)
			palloc0(sizeof(ExprState *) * nelems);
		scanstate->element_qual_natts = (int *)
			palloc0(sizeof(int) * nelems);
		scanstate->element_qual_refs = (GraphRefInfo * *)
			palloc0(sizeof(GraphRefInfo *) * nelems);
		scanstate->element_qual_slots = (TupleTableSlot **)
			palloc0(sizeof(TupleTableSlot *) * nelems);

		for (i = 0; i < nelems; i++)
		{
			GraphElementPattern *gep = (GraphElementPattern *)
				list_nth(path_elements, i);
			ConvertPropRefContext context;
			Node	   *clause = gep->whereClause;

			/*
			 * Element WHERE clauses may reference real (non-scan) relation
			 * Vars; the planner has replaced those with nestloop Params
			 * (replace_nestloop_params in create_graphscan_plan), which the
			 * compiled qual reads from the shared PARAM_EXEC slots.  The
			 * Params are not stripped here.
			 */

			init_prop_ref_context(&context, scanstate, i);

			compile_qual_clause(&context, clause,
								&scanstate->element_quals[i],
								&scanstate->element_qual_natts[i],
								&scanstate->element_qual_refs[i],
								&scanstate->element_qual_slots[i]);
		}
	}

	/* Compile the outer GraphPattern WHERE clause */
	compile_graph_where(scanstate);

	/*
	 * Build same-variable groups for pattern elements that share variable
	 * names
	 */
	{
		List	   *pathelems = NIL;

		scanstate->nsamevar_groups = 0;
		scanstate->samevar_group = (int *)
			palloc0(sizeof(int) * scanstate->num_pi);
		{
			int			pi;

			for (pi = 0; pi < scanstate->num_pi; pi++)
				scanstate->samevar_group[pi] = -1;
		}

		if (node->graph_pattern != NULL &&
			node->graph_pattern->path_pattern_list != NIL)
		{
			int			pi,
						pj;

			pathelems = linitial(node->graph_pattern->path_pattern_list);

			for (pi = 0; pi < scanstate->num_pi; pi++)
			{
				GraphElementPattern *gep_i = (GraphElementPattern *)
					list_nth(pathelems, pi);

				if (gep_i == NULL || gep_i->variable == NULL ||
					scanstate->samevar_group[pi] >= 0)
					continue;

				for (pj = pi + 1; pj < scanstate->num_pi; pj++)
				{
					GraphElementPattern *gep_j = (GraphElementPattern *)
						list_nth(pathelems, pj);

					if (gep_j == NULL || gep_j->variable == NULL)
						continue;

					if (strcmp(gep_i->variable, gep_j->variable) != 0)
						continue;

					/* Same variable, assign to group */
					if (scanstate->samevar_group[pi] < 0)
					{
						scanstate->samevar_group[pi] =
							scanstate->nsamevar_groups;
						scanstate->nsamevar_groups++;
					}
					scanstate->samevar_group[pj] =
						scanstate->samevar_group[pi];
				}
			}
		}

		/* Allocate per-group tracking arrays */
		if (scanstate->nsamevar_groups > 0)
		{
			int			ng = scanstate->nsamevar_groups;

			scanstate->samevar_first_pi = (int *)
				palloc0(sizeof(int) * ng);
			scanstate->samevar_elem_kind = (char *)
				palloc0(sizeof(char) * ng);
			scanstate->samevar_nkeys = (int *)
				palloc0(sizeof(int) * ng);
			scanstate->samevar_key_values = (Datum **)
				palloc0(sizeof(Datum *) * ng);
			scanstate->samevar_key_nulls = (bool **)
				palloc0(sizeof(bool *) * ng);
			scanstate->samevar_tid = (ItemPointerData *)
				palloc0(sizeof(ItemPointerData) * ng);

			/* Find first pi for each group and determine element kind */
			{
				int			g;

				for (g = 0; g < ng; g++)
				{
					int			first_pi = -1;
					int			pi;

					for (pi = 0; pi < scanstate->num_pi; pi++)
					{
						if (scanstate->samevar_group[pi] == g)
						{
							if (first_pi < 0)
								first_pi = pi;
						}
					}

					scanstate->samevar_first_pi[g] = first_pi;

					/* Determine element kind from the pattern */
					if (first_pi >= 0 && pathelems != NIL)
					{
						GraphElementPattern *gep = (GraphElementPattern *)
							list_nth(pathelems, first_pi);

						if (gep->kind == VERTEX_PATTERN)
							scanstate->samevar_elem_kind[g] = 'v';
						else
							scanstate->samevar_elem_kind[g] = 'e';
					}
				}
			}
		}
		else
		{
			scanstate->samevar_first_pi = NULL;
			scanstate->samevar_elem_kind = NULL;
			scanstate->samevar_nkeys = NULL;
			scanstate->samevar_key_values = NULL;
			scanstate->samevar_key_nulls = NULL;
			scanstate->samevar_tid = NULL;
		}
	}

	/* Allocate hop frames */
	if (scanstate->n_hops > 0)
	{
		scanstate->hops = (GraphHopFrame *)
			palloc0(sizeof(GraphHopFrame) * scanstate->n_hops);
	}

	MemoryContextSwitchTo(oldcxt);

	return scanstate;
}

/*
 * Find the attribute number for a property name in a TupleDesc.
 */
static AttrNumber
find_attnum_by_desc(TupleDesc desc, const char *name)
{
	int			i;

	if (desc == NULL || name == NULL)
		return InvalidAttrNumber;

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (!attr->attisdropped && strcmp(NameStr(attr->attname), name) == 0)
			return attr->attnum;
	}
	return InvalidAttrNumber;
}

/*
 * Find the attnum for a property name in a slot's TupleDesc.
 */
static AttrNumber
find_attnum_by_name(TupleTableSlot *slot, const char *name)
{
	if (slot == NULL || slot->tts_tupleDescriptor == NULL)
		return InvalidAttrNumber;

	return find_attnum_by_desc(slot->tts_tupleDescriptor, name);
}

/*
 * Check whether a catalog element (at index ei in element_info) matches
 * the given label expression.  The label expression is a tree of
 * GraphLabelRef nodes (possibly combined via BoolExpr OR).
 * Returns true if the element has any of the referenced labels.
 */
static bool
element_matches_label_expr(GraphScanState * node, int ei, Node *labelexpr)
{
	GraphElementRelInfo *info;

	if (labelexpr == NULL || ei < 0 || ei >= node->num_elements)
		return true;

	info = &node->element_info[ei];

	return label_expr_matches(info, labelexpr);
}

/*
 * Map a pattern position (pi) to a catalog element index by walking
 * the element connectivity graph and applying label filters.
 * Returns -1 if no element found.
 */

/*
 * Tree mutator: recursively replace CollateExpr nodes with their inner
 * argument.  This is needed because ExecInitExprRec doesn't handle
 * CollateExpr, but computed property expressions may contain COLLATE
 * clauses (e.g., 'g2.' || ename COLLATE "C").
 */
static Node *
strip_collate_mutator(Node *node)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, CollateExpr))
	{
		CollateExpr *c = (CollateExpr *) node;

		return strip_collate_mutator((Node *) c->arg);
	}
	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		OpExpr	   *newop = makeNode(OpExpr);
		ListCell   *lc;

		*newop = *op;
		newop->args = NIL;
		foreach(lc, op->args)
			newop->args = lappend(newop->args,
								  strip_collate_mutator((Node *) lfirst(lc)));
		return (Node *) newop;
	}
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) node;
		FuncExpr   *newf = makeNode(FuncExpr);
		ListCell   *lc;

		*newf = *f;
		newf->args = NIL;
		foreach(lc, f->args)
			newf->args = lappend(newf->args,
								 strip_collate_mutator((Node *) lfirst(lc)));
		return (Node *) newf;
	}
	if (IsA(node, RelabelType))
	{
		RelabelType *r = (RelabelType *) node;
		RelabelType *newr = makeNode(RelabelType);

		*newr = *r;
		newr->arg = (Expr *)
			strip_collate_mutator((Node *) r->arg);
		return (Node *) newr;
	}
	if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *c = (CoerceViaIO *) node;
		CoerceViaIO *newc = makeNode(CoerceViaIO);

		*newc = *c;
		newc->arg = (Expr *)
			strip_collate_mutator((Node *) c->arg);
		return (Node *) newc;
	}
	if (IsA(node, BoolExpr))
	{
		BoolExpr   *b = (BoolExpr *) node;
		BoolExpr   *newb = makeNode(BoolExpr);
		ListCell   *lc;

		*newb = *b;
		newb->args = NIL;
		foreach(lc, b->args)
			newb->args = lappend(newb->args,
								 strip_collate_mutator((Node *) lfirst(lc)));
		return (Node *) newb;
	}
	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *s = (ScalarArrayOpExpr *) node;
		ScalarArrayOpExpr *news = makeNode(ScalarArrayOpExpr);
		ListCell   *lc;

		*news = *s;
		news->args = NIL;
		foreach(lc, s->args)
			news->args = lappend(news->args,
								 strip_collate_mutator((Node *) lfirst(lc)));
		return (Node *) news;
	}
	if (IsA(node, NullTest))
	{
		NullTest   *n = (NullTest *) node;
		NullTest   *newn = makeNode(NullTest);

		*newn = *n;
		newn->arg = (Expr *)
			strip_collate_mutator((Node *) n->arg);
		return (Node *) newn;
	}
	if (IsA(node, BooleanTest))
	{
		BooleanTest *b = (BooleanTest *) node;
		BooleanTest *newb = makeNode(BooleanTest);

		*newb = *b;
		newb->arg = (Expr *)
			strip_collate_mutator((Node *) b->arg);
		return (Node *) newb;
	}
	/* For Const, Param, Var, etc. -- return a copy */
	return copyObject(node);
}

/*
 * resolve_graph_property
 *
 *		At runtime, look up the property expression for a computed
 *		property that is not a real column in the vertex table.
 *
 *		The property expression is stored in pg_propgraph_label_property
 *		and was set during CREATE PROPERTY GRAPH.  For a constant
 *		property like "'order'::varchar(10) AS list_type", the expression
 *		is a Const node.
 *
 *		We find the matching element by looking at which physical
 *		table the elem_slot's tuple descriptor corresponds to, then use
 *		the per-element property-expression map built at scan init.
 */
static Datum
resolve_graph_property(GraphScanState * node,
					   Oid propid, TupleTableSlot *slot,
					   bool *isnull, bool is_edge)
{
	char		element_kind = is_edge ? 'e' : 'v';
	int			ei;

	*isnull = true;

	if (slot == NULL || slot->tts_tupleDescriptor == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("graph property resolution called with no element slot")));

	/* Find which element OID this slot corresponds to */
	for (ei = 0; ei < node->num_elements; ei++)
	{
		GraphElementRelInfo *info = &node->element_info[ei];

		if (info->element_kind != element_kind)
			continue;
		if (info->heap_rel && info->tupdesc->tdtypeid ==
			slot->tts_tupleDescriptor->tdtypeid)
			break;
	}

	/*
	 * A slot that matches no element of the graph is an internal
	 * inconsistency (the per-row paths only pass element slots built from
	 * element_info).  Surface it as an error rather than silently NULLing the
	 * property, which would mask a bug.  Note: a property that is missing on
	 * the *matched* element (but present on another candidate label) is NOT
	 * an error -- for multi-label elements that is legitimate and resolves to
	 * SQL NULL, so the loop below keeps that behavior.
	 */
	if (ei >= node->num_elements)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("graph scan could not identify the element for property resolution")));

	/* Find the property expression cached for this element */
	for (GraphPropExpr * pe = node->element_info[ei].prop_exprs;
		 pe != NULL; pe = pe->next)
	{
		if (pe->propid == propid && pe->exprstate != NULL)
		{
			ExprContext *econtext;

			/*
			 * The expression was already compiled at scan init (see
			 * compile_property_exprs in ExecInitGraphScan) so that this hot
			 * path never runs ExecInitExpr or strip_collate_mutator per row.
			 * The compiled Var references read ecxt_scantuple, which is set
			 * to the element slot below.
			 */
			econtext = node->ss.ps.ps_ExprContext;
			econtext->ecxt_scantuple = slot;
			ResetExprContext(econtext);
			return ExecEvalExpr(pe->exprstate, econtext, isnull);
		}
	}

	return (Datum) 0;
}

/*
 * Convert a GraphPropertyRef node to a Var(OUTER_VAR, attnum) with the
 * appropriate attribute number for reading the property from the element
 * tuple slot.  Also replaces CollateExpr nodes with RelabelType (with the
 * same collation) because ExecInitExprRec does not handle CollateExpr.
 * Returns a new copy of the expression tree.
 */
static Node *
convert_GraphPropertyRef_to_Var_internal(Node *node,
										 ConvertPropRefContext * context)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, GraphPropertyRef))
	{
		GraphPropertyRef *gpr = (GraphPropertyRef *) node;
		char	   *propname;
		AttrNumber	attnum = InvalidAttrNumber;
		int			elem_idx = context->fixed_elem_idx;

		if (context->graph_qual_mode)
		{
			/*
			 * Qual compilation: assign this ref a virtual attribute number in
			 * the shared virtual slot and remember its source element /
			 * property for runtime filling by name.
			 */
			int			vatnum = context->nqual_atts + 1;
			GraphRefInfo *ri;

			context->nqual_atts = vatnum;
			ri = &context->qual_refs[vatnum - 1];
			ri->elem_idx = (context->fixed_elem_idx >= 0)
				? context->fixed_elem_idx
				: find_ref_elem_index((Node *) gpr, context->scanstate);
			ri->propid = gpr->propid;
			ri->vartype = gpr->typeId;
			ri->typmod = gpr->typmod;
			ri->collation = gpr->collation;
			if (ri->elem_idx >= 0 && ri->elem_idx < context->scanstate->num_pi)
			{
				GraphElementPattern *gep =
					GetPatternElement(context->scanstate, ri->elem_idx);

				ri->is_edge = (gep != NULL && IS_EDGE_PATTERN(gep->kind));
			}
			else
				ri->is_edge = false;

			return (Node *) makeVar(OUTER_VAR, vatnum,
									gpr->typeId, gpr->typmod,
									gpr->collation, 0);
		}

		/* Resolve the pattern element this ref belongs to */
		if (elem_idx < 0)
			elem_idx = find_ref_elem_index((Node *) gpr,
										   context->scanstate);
		if (elem_idx >= 0 && elem_idx < context->scanstate->num_pi)
		{
			GraphElementPattern *gep = GetPatternElement(context->scanstate,
														 elem_idx);
			List	   *candidates = NIL;
			ListCell   *lc;

			/*
			 * Resolve the element's physical table attribute for this
			 * property, restricted to the candidate backing tables of the
			 * element the reference belongs to.  Different elements may use
			 * the same property name at different physical positions (e.g.
			 * "data" is attribute 4 of items but attribute 2 of versions), so
			 * a global name search across every element would pick the wrong
			 * attribute.  With multi-label elements, take the first candidate
			 * that has the column.
			 */
			collect_candidate_elements(context->scanstate, gep, &candidates);
			propname = get_propgraph_property_name(gpr->propid);
			foreach(lc, candidates)
			{
				int			eii = lfirst_int(lc);
				TupleDesc	tupdesc = context->scanstate->element_info[eii].tupdesc;

				if (tupdesc == NULL)
					continue;
				attnum = find_attnum_by_desc(tupdesc, propname);
				if (attnum != InvalidAttrNumber)
					break;
			}

			/*
			 * If the property isn't a physical column but is a computed
			 * property, the element slot doesn't carry it.  Map it to a
			 * synthetic attnum beyond the element's natts so that the
			 * virtual-tuple machinery (graph-level) / callers can distinguish
			 * it.  For element-level clauses this cannot happen (the plan
			 * validates refs) so leave attnum=1 fallback.
			 */
		}

		if (attnum == InvalidAttrNumber)
			attnum = 1;			/* computed property: caller resolves at
								 * runtime */

		return (Node *) makeVar(OUTER_VAR, attnum,
								gpr->typeId, gpr->typmod,
								gpr->collation, 0);
	}

	/*
	 * ExecInitExprRec does not handle CollateExpr; replace it with a
	 * RelabelType carrying the same collation, mirroring what
	 * eval_const_expressions does.
	 */
	if (IsA(node, CollateExpr))
	{
		CollateExpr *c = (CollateExpr *) node;
		Node	   *newarg;

		newarg = convert_GraphPropertyRef_to_Var_internal(
														  (Node *) c->arg, context);
		return (Node *) makeRelabelType((Expr *) newarg,
										exprType(newarg),
										exprTypmod(newarg),
										c->collOid,
										COERCE_IMPLICIT_CAST);
	}

	/*
	 * Recurse generically through every expression node type so that any
	 * GraphPropertyRef nested anywhere in the clause (e.g. under a NullTest
	 * from IS [NOT] NULL, a FieldSelect, an ArrayExpr, ...) is converted and
	 * counted, consistent with the number of refs counted up front in
	 * compile_qual_clause.  Using expression_tree_mutator rather than a
	 * hand-rolled allowlist keeps the two passes in agreement for all node
	 * types.
	 */
	return expression_tree_mutator(node,
								   convert_GraphPropertyRef_to_Var_internal,
								   context);
}

static void
convert_GraphPropertyRef_to_Var(Node **node_p, int elem_idx,
								GraphScanState * scanstate)
{
	ConvertPropRefContext context;

	init_prop_ref_context(&context, scanstate, elem_idx);
	*node_p = convert_GraphPropertyRef_to_Var_internal(*node_p, &context);
}

/*
 * Compile one WHERE clause (element-level or graph-level) into an ExprState.
 *
 * Every GraphPropertyRef is mapped to a distinct virtual attribute number in
 * the produced virtual slot; refs[] records the source pattern element +
 * property per attribute.  CollateExpr nodes are replaced with RelabelType and
 * other standard expression nodes are passed through, so the whole clause
 * compiles to an ExprState evaluable via ExecEvalExpr.
 *
 * fixed_elem_idx >= 0: element-level clause (all refs belong to that pattern
 * element).  fixed_elem_idx < 0: graph-level clause (refs resolved by
 * elvarname).
 */
static void
compile_qual_clause(ConvertPropRefContext * context, Node *clause,
					ExprState **expr_out, int *natts_out,
					GraphRefInfo * *refs_out, TupleTableSlot **slot_out)
{
	int			nrefs;
	TupleDesc	tupdesc;
	List	   *refs = NIL;
	Node	   *converted;

	/* Count GraphPropertyRefs (all of them, regardless of variable name) */
	{
		GraphExprScanContext ctx;

		ctx.varname = NULL;
		ctx.refs = &refs;
		ctx.varno = 0;
		ctx.scanstate = NULL;
		ctx.found_elem_idx = -1;
		ctx.found_varattno = -1;
		ctx.first_match = false;
		(void) graph_expr_scanner(clause, &ctx);
	}
	nrefs = list_length(refs);

	if (nrefs == 0)
	{
		/* No property refs: compile as-is, no virtual slot */
		*expr_out = ExecInitExpr((Expr *) clause,
								 (PlanState *) context->scanstate);
		*natts_out = 0;
		*refs_out = NULL;
		*slot_out = NULL;
		return;
	}

	*refs_out = (GraphRefInfo *) palloc0(sizeof(GraphRefInfo) * nrefs);

	context->graph_qual_mode = true;
	context->nqual_atts = 0;
	context->qual_refs = *refs_out;
	converted = convert_GraphPropertyRef_to_Var_internal(clause, context);
	Assert(context->nqual_atts == nrefs);

	/* Build the virtual tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(nrefs);
	for (int i = 0; i < nrefs; i++)
	{
		TupleDescInitEntry(tupdesc, (AttrNumber) (i + 1), NULL,
						   (*refs_out)[i].vartype,
						   (*refs_out)[i].typmod, 0);
		TupleDescInitEntryCollation(tupdesc, (AttrNumber) (i + 1),
									(*refs_out)[i].collation);
	}

	*slot_out = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

	*expr_out = ExecInitExpr((Expr *) converted, (PlanState *) context->scanstate);
	*natts_out = nrefs;
}

static void
compile_graph_where(GraphScanState * scanstate)
{
	GraphScan  *plan = (GraphScan *) scanstate->ss.ps.plan;
	Node	   *clause = plan->graph_pattern->whereClause;
	ConvertPropRefContext context;

	init_prop_ref_context(&context, scanstate, -1);

	if (clause == NULL)
	{
		scanstate->graph_qual = NULL;
		scanstate->graph_qual_natts = 0;
		scanstate->graph_qual_refs = NULL;
		scanstate->qual_virt_slot = NULL;
		return;
	}

	/*
	 * Graph-level WHERE may reference real (non-scan) relation Vars; the
	 * planner has replaced those with nestloop Params, which the compiled
	 * qual reads from the shared PARAM_EXEC slots.
	 */

	compile_qual_clause(&context, clause,
						&scanstate->graph_qual,
						&scanstate->graph_qual_natts,
						&scanstate->graph_qual_refs,
						&scanstate->qual_virt_slot);
}

/*
 * Try to read a property as a direct physical column of the element's
 * backing table: first via the cached propid->attnum resolution, then by
 * column name.  Returns true with the value and null flag written when a
 * physical column exists; false when the caller must fall back to the
 * label-aware/computed property expression.
 */
static bool
fill_physical_prop_value(GraphScanState * node, TupleTableSlot *elem_slot,
						 Oid propid, const char *propname, bool materialize,
						 Datum *value, bool *isnull)
{
	AttrNumber	attnum;

	if (OidIsValid(propid))
	{
		attnum = find_prop_attnum(node, elem_slot, propid);
		if (attnum == InvalidAttrNumber)
			attnum = find_attnum_by_name(elem_slot, propname);
	}
	else
		attnum = find_attnum_by_name(elem_slot, propname);

	if (attnum == InvalidAttrNumber)
		return false;

	if (materialize)
		ExecMaterializeSlot(elem_slot);
	*value = slot_getattr(elem_slot, attnum, isnull);
	return true;
}

/*
 * Fill a virtual outer tuple from element slots.  Each virtual attribute is
 * sourced from the recorded pattern element's property: the physical column
 * with that name if present, otherwise the computed property resolved through
 * the cached model.  Returns the filled slot (NULL if none).
 *
 * If forced_elem_slot is given (element-level clause) every attribute is
 * sourced from it; otherwise each attribute is sourced from slot_for_elem[]
 * indexed by the recorded pattern element.
 */
static TupleTableSlot *
fill_qual_slot(GraphScanState * node, TupleTableSlot *virt_slot,
			   int natts, const GraphRefInfo * refs, int fixed_elem_idx,
			   TupleTableSlot *forced_elem_slot)
{
	if (virt_slot == NULL)
		return NULL;

	ExecClearTuple(virt_slot);
	init_slot_values_null(virt_slot, natts);

	for (int at = 0; at < natts; at++)
	{
		const		GraphRefInfo *ri = &refs[at];
		int			pi = (fixed_elem_idx >= 0) ? fixed_elem_idx : ri->elem_idx;
		TupleTableSlot *elem_slot = forced_elem_slot;
		bool		isnull = true;

		if (elem_slot == NULL)
		{
			if (pi < 0 || pi >= node->num_pi ||
				node->slot_for_elem == NULL ||
				node->slot_for_elem[pi] == NULL)
				continue;
			elem_slot = node->slot_for_elem[pi];
		}

		if (fill_physical_prop_value(node, elem_slot, ri->propid,
									 get_propgraph_property_name(ri->propid),
									 true, &virt_slot->tts_values[at], &isnull))
			virt_slot->tts_isnull[at] = isnull;
		else
		{
			virt_slot->tts_values[at] =
				resolve_graph_property(node, ri->propid, elem_slot, &isnull,
									   ri->is_edge);
			virt_slot->tts_isnull[at] = isnull;
		}
	}

	ExecStoreVirtualTuple(virt_slot);
	return virt_slot;
}

/*
 * Fill the graph-level WHERE's virtual outer tuple from the current element
 * slots, and bind it as ecxt_outertuple.
 */
static void
fill_graph_qual_slot(GraphScanState * node)
{
	if (node->qual_virt_slot == NULL)
		return;

	if (fill_qual_slot(node, node->qual_virt_slot,
					   node->graph_qual_natts, node->graph_qual_refs, -1,
					   NULL)
		!= NULL)
		node->ss.ps.ps_ExprContext->ecxt_outertuple = node->qual_virt_slot;
}


/*
 * Evaluate a compiled boolean qual used for graph clause/RLS filtering.  A
 * NULL result is treated as "not matched".  Resets the expression context
 * first, as every caller does.
 */
static bool
eval_qual_bool(ExprContext *econtext, ExprState *qual)
{
	bool		isnull;

	ResetExprContext(econtext);
	return DatumGetBool(ExecEvalExpr(qual, econtext, &isnull)) && !isnull;
}

/*
 * Evaluate the graph-level WHERE clause using the compiled ExprState.
 */
static bool
evaluate_graph_where(GraphScanState * node)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	if (node->graph_qual == NULL)
		return true;

	fill_graph_qual_slot(node);

	return eval_qual_bool(econtext, node->graph_qual);
}

/*
 * Evaluate an element-level WHERE clause using its compiled ExprState.
 * Before evaluation, ecxt_outertuple must point at the element's slot so the
 * converted Var(OUTER_VAR, attnum) reads the property.
 *
 * Returns true if the clause passes (or there is no clause), false otherwise.
 */ static bool
evaluate_element_where(GraphScanState * node, int elem_idx,
					   TupleTableSlot *elem_slot)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	ExprState  *qual;

	if (elem_idx < 0 || elem_idx >= node->num_pi)
		return true;
	qual = node->element_quals[elem_idx];
	if (qual == NULL)
		return true;

	/*
	 * Fill the element's virtual qual slot from the current element tuple by
	 * property name (physical column or computed property), then evaluate.
	 */
	if (fill_qual_slot(node, node->element_qual_slots[elem_idx],
					   node->element_qual_natts[elem_idx],
					   node->element_qual_refs[elem_idx], elem_idx,
					   elem_slot) == NULL)
	{
		/* No property refs: still need the element slot as outer tuple */
		econtext->ecxt_outertuple = elem_slot;
	}
	else
		econtext->ecxt_outertuple = node->element_qual_slots[elem_idx];
	econtext->ecxt_scantuple = elem_slot;

	return eval_qual_bool(econtext, qual);
}

/*
 * Build the per-element RLS quals at executor init.
 *
 * The plan carries per-backing-table RLS quals (GraphRLSQual, quals' Vars on
 * OUTER_VAR).  For each element the scan will touch, compile the matching
 * table's predicates into a single ExprState stored on
 * element_info[ei].rls_qual; rows failing it are filtered at scan time.
 */
static void
compile_element_rls(GraphScanState * scanstate)
{
	GraphScan  *plan = (GraphScan *) scanstate->ss.ps.plan;
	ListCell   *lc;

	if (plan->rls_quals == NIL)
		return;

	foreach(lc, plan->rls_quals)
	{
		GraphRLSQual *rlsq = lfirst_node(GraphRLSQual, lc);
		Oid			relid = rlsq->relid;
		List	   *quals = rlsq->quals;
		Node	   *clause;
		int			ei;

		/* Combine the (permissive/restrictive) quals with AND */
		if (quals == NIL)
			clause = (Node *) makeConst(BOOLOID, -1, InvalidOid,
										sizeof(bool), BoolGetDatum(false),
										false, true);
		else if (list_length(quals) == 1)
			clause = (Node *) linitial(quals);
		else
			clause = (Node *) makeBoolExpr(AND_EXPR, quals, -1);

		for (ei = 0; ei < scanstate->num_elements; ei++)
		{
			if (scanstate->element_info[ei].table_oid != relid)
				continue;
			if (scanstate->element_info[ei].rls_qual != NULL)
				continue;

			scanstate->element_info[ei].rls_qual =
				ExecInitExpr((Expr *) clause, (PlanState *) scanstate);
		}
	}
}

/*
 * Evaluate the row-security qual for an element's backing table against a
 * scanned row.  The qual's Vars reference OUTER_VAR (see
 * get_graph_row_security_quals) so they read from ecxt_outertuple, exactly
 * like the converted element WHERE quals.
 *
 * Returns true if the row passes (or there is no RLS qual), false otherwise.
 */
static bool
evaluate_element_rls(GraphScanState * node, GraphElementRelInfo * info,
					 TupleTableSlot *slot)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	if (info == NULL || info->rls_qual == NULL)
		return true;

	econtext->ecxt_outertuple = slot;
	econtext->ecxt_scantuple = slot;

	return eval_qual_bool(econtext, info->rls_qual);
}

/*
 * Check whether the given property (identified by propid) exists on ALL
 * candidate element tables for pattern element pi.  Returns true if the
 * property is present on every candidate, false if at least one candidate
 * lacks it.  Used to determine whether a direct slot read is safe, or
 * whether a label-aware runtime check is needed.
 */
static bool
is_property_on_all_candidates(GraphScanState * node, int pi, Oid propid)
{
	GraphScan  *plan = (GraphScan *) node->ss.ps.plan;
	List	   *path = linitial(plan->graph_pattern->path_pattern_list);
	GraphElementPattern *gep = (GraphElementPattern *) list_nth(path, pi);
	List	   *label_oids = NIL;
	List	   *candidates = NIL;
	ListCell   *lc;

	if (gep == NULL)
		return true;

	if (gep->labelexpr != NULL)
		label_oids = get_label_oids_for_labelexpr(gep->labelexpr);

	/* Collect matching candidate elements from the built model */
	collect_candidate_elements(node, gep, &candidates);

	if (candidates == NIL)
		return true;

	foreach(lc, candidates)
	{
		if (!element_has_property(node, lfirst_int(lc), propid,
								  label_oids))
			return false;
	}
	return true;
}

/*
 * Build the column mapping arrays from the plan target list and
 * graph_table_columns.  Each entry maps a plan target list position
 * to the pattern element and table attribute that provides its value.
 */
static void
init_column_mapping(GraphScanState * node, EState *estate)
{
	GraphScan  *plan = (GraphScan *) node->ss.ps.plan;
	List	   *gtcols;
	int			rti = plan->scan.scanrelid;
	int			gtcol_count;
	int			ntlist;
	int			ci;
	TargetEntry *gte;
	int			varattno;

	gtcols = plan->graph_table_columns;
	gtcol_count = list_length(gtcols);
	ntlist = list_length(plan->scan.plan.targetlist);

	/*
	 * The mapping arrays are indexed by graph table column position
	 * (0-based), matching the scan slot's TupleDesc (built from
	 * graph_table_columns) and MaterializeCurrentPathSlot /
	 * fill_one_slot_attr which iterate over those columns.  The plan target
	 * list may list the columns in a different order, so it must not be used
	 * to index them.
	 */
	node->ncols = gtcol_count;
	node->col_elem_index = (int *) palloc0(sizeof(int) * gtcol_count);
	node->col_propname = (char **) palloc0(sizeof(char *) * gtcol_count);
	node->col_propid = (Oid *) palloc0(sizeof(Oid) * gtcol_count);
	node->col_prop_unsafe = (bool *) palloc0(sizeof(bool) * gtcol_count);

	for (ci = 0; ci < gtcol_count; ci++)
	{
		node->col_elem_index[ci] = -1;
		node->col_propid[ci] = InvalidOid;
	}

	/* Map each target list entry to its graph table column position */
	for (ci = 0; ci < ntlist; ci++)
	{
		TargetEntry *te = (TargetEntry *)
			list_nth(plan->scan.plan.targetlist, ci);

		/*
		 * Determine which graph table column index this target list entry
		 * corresponds to.
		 */
		if (IsA(te->expr, Var) &&
			((Var *) te->expr)->varno == rti)
		{
			varattno = ((Var *) te->expr)->varattno;
		}
		else
		{
			/*
			 * Non-Var target list entries (e.g. FuncExpr, RelabelType) still
			 * reference a graph table column position. Walk the expression
			 * tree to find Var references.
			 */
			varattno = find_varattno_in_expr((Node *) te->expr, rti);
		}

		if (varattno <= 0 || varattno > gtcol_count)
			continue;

		gte = (TargetEntry *) list_nth(gtcols, varattno - 1);

		{
			Node	   *gcol = (Node *) gte->expr;

			/* Unwrap wrapper expressions (FuncExpr handled separately) */
			while (IsA(gcol, FuncExpr))
				gcol = (Node *) linitial(((FuncExpr *) gcol)->args);
			gcol = strip_expr_wrappers(gcol);

			if (IsA(gcol, Const))
			{
				/*
				 * Constant columns are compiled to an ExprState in the
				 * column-expression pass (see ExecInitGraphScan); nothing to
				 * record here.
				 */
			}
			else if (IsA(gcol, GraphPropertyRef))
			{
				GraphPropertyRef *gpr = (GraphPropertyRef *) gcol;
				const char *varname = gpr->elvarname;
				char	   *propname;
				int			pi;
				List	   *path_elements;
				int			gci = varattno - 1;

				propname = get_propgraph_property_name(gpr->propid);
				node->col_propname[gci] = pstrdup(propname);
				node->col_propid[gci] = gpr->propid;
				path_elements = linitial(plan->graph_pattern->path_pattern_list);

				for (pi = 0; pi < list_length(path_elements); pi++)
				{
					GraphElementPattern *gep = (GraphElementPattern *)
						list_nth(path_elements, pi);

					if (!gep->variable ||
						strcmp(gep->variable, varname) != 0)
						continue;

					node->col_elem_index[gci] = pi;
					break;
				}

				/*
				 * Mark as unsafe if the property does not exist on ALL
				 * candidate label tables.  At runtime, we will use
				 * resolve_graph_property (which checks label-property
				 * bindings) instead of a direct physical read.
				 */
				if (node->col_elem_index[gci] >= 0 &&
					!is_property_on_all_candidates(node,
												   node->col_elem_index[gci],
												   node->col_propid[gci]))
					node->col_prop_unsafe[gci] = true;
			}
		}
	}
}

/* ----------------------------------------------------------------
 *	 init_start_vertex
 *
 *		Begin scanning the start vertex table.  Starts with the
 *		first matching vertex table at table_index=0.
 * ----------------------------------------------------------------
 */
static void
init_start_vertex(GraphScanState * node)
{
	GraphElementPattern *elem = GetPatternElement(node, 0);
	GraphElementRelInfo *rel_info;

	node->start_table_index = 0;
	node->current_hop = -1;

	if (elem == NULL)
		return;

	rel_info = GetElementRelInfo(node, elem, 0);

	/* Start a scan on the first matching vertex table */
	if (rel_info)
	{
		check_table_privilege(rel_info->table_oid);

		/*
		 * Create the slot before element_scan_start so it's allocated in the
		 * executor's memory context (before SPI_connect switches contexts for
		 * views).
		 */
		node->start_vertex_slot = element_make_slot(rel_info);
		element_scan_start(rel_info, node->ss.ps.state->es_snapshot);
	}
}

/* ----------------------------------------------------------------
 *	 start_vertex_advance
 *
 *		Advance to the next start vertex.  Handles multi-table
 *		iteration: when one vertex table is exhausted, tries the
 *		next matching vertex table.  Returns true if a new start
 *		vertex was found, false if all tables are exhausted.
 * ----------------------------------------------------------------
 */
static bool
start_vertex_advance(GraphScanState * node)
{
	GraphElementPattern *elem = GetPatternElement(node, 0);
	GraphElementRelInfo *rel_info;

	if (elem == NULL)
		return false;

	for (;;)
	{
		rel_info = GetElementRelInfo(node, elem, node->start_table_index);
		if (rel_info == NULL)
		{
			/* No more matching vertex tables */
			return false;
		}

		/* Try to get the next tuple from the current table's scan */
		if (node->start_vertex_slot != NULL &&
			element_scan_next(rel_info, node->start_vertex_slot))
		{
			/* extract PK values and check WHERE clause for vertex */
			node->n_start_keys = rel_info->nkey_attnums;
			set_datum_pair(rel_info->nkey_attnums,
						   &node->start_vertex_values,
						   &node->start_vertex_nulls, NULL, NULL);
			for (int ki = 0; ki < rel_info->nkey_attnums; ki++)
				node->start_vertex_values[ki] =
					slot_getattr(node->start_vertex_slot,
								 rel_info->key_attnums[ki],
								 &node->start_vertex_nulls[ki]);

			/* Check RLS on the start vertex row, then the WHERE clause */
			if (!evaluate_element_rls(node, rel_info,
									  node->start_vertex_slot))
				continue;
			if (!evaluate_element_where(node, 0,
										node->start_vertex_slot))
				continue;

			/* Materialize so the slot survives across scan advances */
			ExecMaterializeSlot(node->start_vertex_slot);

			/* Store PK identity for same-var tracking if pi=0 is shared */
			store_start_vertex_identity(node);

			return true;
		}

		/* Exhausted this table, close scan and move to next table */
		if (node->start_vertex_slot != NULL)
		{
			element_scan_end(rel_info);
			ExecDropSingleTupleTableSlot(node->start_vertex_slot);
			node->start_vertex_slot = NULL;
		}

		/* Advance to next matching vertex table */
		node->start_table_index++;
		rel_info = GetElementRelInfo(node, elem, node->start_table_index);
		if (rel_info == NULL)
			return false;

		/* Start scanning the next table */
		check_table_privilege(rel_info->table_oid);
		node->start_vertex_slot = element_make_slot(rel_info);
		element_scan_start(rel_info, node->ss.ps.state->es_snapshot);
	}
}

/* ----------------------------------------------------------------
 *	 find_edge_table_for_vertex
 *
 *		Find the edge_table_index-th matching edge catalog element
 *		for a given vertex element OID, filtered by label expression
 *		and traversal direction.
 *
 *		For OUTGOING or undirected pass 0: matches edges where
 *		src_vertex_elemid == elemoid (the vertex is the source).
 *		For INCOMING or undirected pass 1: matches edges where
 *		dest_vertex_elemid == elemoid (the vertex is the destination).
 *
 *		Returns the catalog element index, or -1 if no match.
 * ----------------------------------------------------------------
 */
static int
find_edge_table_for_vertex(GraphScanState * node, Oid elemoid,
						   Node *labelexpr, int table_index,
						   EdgeDirection dir, int scan_pass)
{
	int			count = 0;
	int			i;
	bool		is_incoming = edge_is_incoming(dir, scan_pass);

	for (i = 0; i < node->num_elements; i++)
	{
		if (node->element_info[i].element_kind != 'e')
			continue;

		if (is_incoming)
		{
			if (node->element_info[i].dest_vertex_elemid != elemoid)
				continue;
		}
		else
		{
			if (node->element_info[i].src_vertex_elemid != elemoid)
				continue;
		}

		if (labelexpr != NULL &&
			!element_matches_label_expr(node, i, labelexpr))
			continue;

		if (count == table_index)
			return i;
		count++;
	}
	return -1;
}

/* ----------------------------------------------------------------
 *	 vle_find_next_edge
 *
 *	 Find the next edge from the given source vertex, scanning
 *	 edge tables and handling undirected passes.  The source
 *	 vertex identity is passed as parameters (src_elemid,
 *	 src_values, etc.) rather than read from hop->from_vertex_*.
 *	 On entry, *start_table_idx and *start_scan_pass indicate where
 *	 to resume scanning; on return (found) they are updated to the
 *	 resume position.
 *
 *		On success, sets *dest_elemid, *dest_nkeys, *dest_values,
 *		*dest_nulls with the matched destination vertex, and returns
 *		true.  On exhaustion, returns false.
 *
 *		Edge scan state is stored on the GraphHopFrame (edge_scan,
 *		edge_slot), shared across all stack frame levels.
 * ----------------------------------------------------------------
 */
static bool
vle_find_next_edge(GraphScanState * node,
				   Oid src_elemid,
				   int src_nkeys, Datum *src_values,
				   bool *src_nulls,
				   int *start_table_idx, int *start_scan_pass,
				   int edge_elem_idx, EdgeDirection dir,
				   GraphHopFrame * hop,
				   Oid *dest_elemid,
				   int *dest_nkeys, Datum **dest_values,
				   bool **dest_nulls)
{
	GraphElementPattern *edge_elem = GetPatternElement(node, edge_elem_idx);
	GraphElementRelInfo *edge_info;

	if (src_elemid == InvalidOid)
		return false;

	for (;;)
	{
		int			edge_cat_idx;

		/* Find which edge catalog table to scan */
		edge_cat_idx = find_edge_table_for_vertex(node, src_elemid,
												  edge_elem ? edge_elem->labelexpr : NULL,
												  *start_table_idx,
												  dir, *start_scan_pass);

		if (edge_cat_idx < 0)
		{
			/* No more matching edge tables. Try undirected second pass */
			if (dir == GRAPH_DIR_UNDIRECTED && *start_scan_pass == 0)
			{
				*start_scan_pass = 1;
				*start_table_idx = 0;
				hop_close_edge_scan(hop);
				continue;
			}
			return false;
		}

		edge_info = &node->element_info[edge_cat_idx];

		/* Start a scan if not already started for this table */
		if (is_plain_relation(edge_info))
		{
			if (hop->edge_scan == NULL ||
				!hop->edge_slot ||
				hop->edge_slot->tts_tupleDescriptor != edge_info->tupdesc)
			{
				hop_close_edge_scan(hop);

				check_table_privilege(edge_info->table_oid);
				hop->edge_scan = table_beginscan(edge_info->heap_rel,
												 node->ss.ps.state->es_snapshot,
												 0, NULL, SO_NONE);
				hop->edge_slot =
					MakeSingleTupleTableSlot(edge_info->tupdesc,
											 table_slot_callbacks(edge_info->heap_rel));
			}
		}
		else
		{
			/* Non-heap edge table: use element_scan abstraction */
			if (hop->edge_slot == NULL)
			{
				hop_close_edge_scan(hop);
				element_scan_start(edge_info, node->ss.ps.state->es_snapshot);
				hop->edge_slot = element_make_slot(edge_info);
				hop->edge_scan = NULL;
			}
		}

		/* Scan through edge tuples */
		{
			bool		got_edge = false;

			while (!got_edge)
			{
				bool		got_tuple;

				if (is_plain_relation(edge_info))
				{
					got_tuple = table_scan_getnextslot(hop->edge_scan,
													   ForwardScanDirection,
													   hop->edge_slot);
				}
				else
				{
					got_tuple = element_scan_next(edge_info, hop->edge_slot);
				}

				if (!got_tuple)
					break;

				{
					bool		match = false;
					int			fk_attr_count;

					if (dir == GRAPH_DIR_UNDIRECTED && *start_scan_pass == 1 &&
						hop->undirected_has_seen &&
						hop->undirected_seen_edge_idx == edge_cat_idx &&
						is_plain_relation(edge_info) &&
						ItemPointerEquals(&hop->edge_slot->tts_tid,
										  &hop->undirected_seen_tid))
						continue;

					/* Determine which FK columns to check based on direction */
					if (edge_is_incoming(dir, *start_scan_pass))
						fk_attr_count = edge_info->ndst_fk_attnums;
					else
						fk_attr_count = edge_info->nsrc_fk_attnums;

					if (fk_attr_count > 0)
					{
						bool		incoming = edge_is_incoming(dir,
																*start_scan_pass);

						/*
						 * The FK must not have more columns than the source
						 * vertex key provides (the original loop treated any
						 * run past src_nkeys as a non-match).
						 */
						if (fk_attr_count <= src_nkeys)
							match = slot_matches_values(hop->edge_slot,
														fk_attr_count,
														incoming ?
														edge_info->dst_fk_attnums :
														edge_info->src_fk_attnums,
														incoming ?
														edge_info->dst_fk_eq :
														edge_info->src_fk_eq,
														src_values, src_nulls);
						else
							match = false;
					}
					else
						match = true;

					if (!match)
						continue;

					/* Check RLS on the edge row, then the WHERE clause */
					if (!evaluate_element_rls(node, edge_info,
											  hop->edge_slot))
						continue;
					if (edge_elem_idx >= 0 &&
						edge_elem_idx < node->num_pi &&
						!evaluate_element_where(node, edge_elem_idx,
												hop->edge_slot))
						continue;


					/* Check same-variable identity for edges */
					if (!check_samevar_edge_identity(node, edge_elem_idx, hop))
						continue;

					/*
					 * Found a matching edge. Materialize and fetch dest
					 * vertex
					 */
					ExecMaterializeSlot(hop->edge_slot);

					/* Extract target vertex keys */
					{
						int			n_fk_cols;
						int			dest_vert_idx;

						if (edge_is_incoming(dir, *start_scan_pass))
						{
							n_fk_cols = edge_info->nsrc_fk_attnums;
							dest_vert_idx =
								node->src_vertex_idx[edge_cat_idx];
						}
						else
						{
							n_fk_cols = edge_info->ndst_fk_attnums;
							dest_vert_idx =
								node->dest_vertex_idx[edge_cat_idx];
						}

						*dest_nkeys = n_fk_cols;
						set_datum_pair(n_fk_cols, dest_values, dest_nulls,
									   NULL, NULL);
						ExtractTargetVertexKeys(hop->edge_slot, edge_info,
												dir, *start_scan_pass,
												n_fk_cols,
												*dest_values,
												*dest_nulls);

						if (dest_vert_idx >= 0)
						{
							GraphElementRelInfo *dest_info =
								&node->element_info[dest_vert_idx];
							TupleTableSlot *dest_slot;

							/* Fetch dest vertex via PK lookup */
							dest_slot = fetch_vertex_by_pk(dest_info,
														   *dest_values,
														   *dest_nulls,
														   n_fk_cols,
														   node->ss.ps.state);
							if (dest_slot == NULL)
								continue;

							/* Check RLS on the destination vertex row */
							if (!evaluate_element_rls(node, dest_info,
													  dest_slot))
							{
								ExecDropSingleTupleTableSlot(dest_slot);
								continue;
							}

							/*
							 * Validate that destination vertex matches the
							 * label expression of the next pattern element.
							 */
							{
								int			next_elem_idx = edge_elem_idx + 1;

								if (next_elem_idx < node->num_pi)
								{
									GraphElementPattern *next_elem =
										GetPatternElement(node, next_elem_idx);

									if (next_elem && next_elem->labelexpr &&
										!element_matches_label_expr(
																	node, dest_vert_idx,
																	next_elem->labelexpr))
									{
										ExecDropSingleTupleTableSlot(dest_slot);
										continue;
									}
								}
							}

							/*
							 * Check WHERE clause on destination vertex
							 * element
							 */
							{
								int			dest_elem_idx = edge_elem_idx + 1;

								if (dest_elem_idx < node->num_pi &&
									!evaluate_element_where(node, dest_elem_idx,
															dest_slot))
								{
									ExecDropSingleTupleTableSlot(dest_slot);
									continue;
								}
							}

							/* Check same-variable vertex identity */
							{
								int			dest_elem_idx = edge_elem_idx + 1;

								if (!check_samevar_vertex_identity(
																   node, dest_elem_idx, hop,
																   dest_info))
								{
									ExecDropSingleTupleTableSlot(dest_slot);
									continue;
								}
							}

							/* Store the dest vertex slot (discard old one) */
							if (hop->to_vertex_slot)
								ExecDropSingleTupleTableSlot(hop->to_vertex_slot);
							hop->to_vertex_slot = dest_slot;
							{
								/*
								 * For incoming traversal, the "to" vertex is
								 * the edge's source element.
								 */
								*dest_elemid =
									edge_is_incoming(dir, *start_scan_pass) ?
									node->element_info[edge_cat_idx].src_vertex_elemid :
									node->element_info[edge_cat_idx].dest_vertex_elemid;
							}

							/*
							 * For undirected scans: store the TID of an edge
							 * accepted on pass 0.
							 */
							if (dir == GRAPH_DIR_UNDIRECTED &&
								*start_scan_pass == 0 && hop->edge_slot &&
								is_plain_relation(&node->element_info[edge_cat_idx]))
							{
								if (node->element_info[edge_cat_idx].src_vertex_elemid ==
									node->element_info[edge_cat_idx].dest_vertex_elemid)
								{
									ItemPointerCopy(&hop->edge_slot->tts_tid,
													&hop->undirected_seen_tid);
									hop->undirected_seen_edge_idx = edge_cat_idx;
									hop->undirected_has_seen = true;
								}
							}

							return true;
						}
					}
				}
			}					/* end of processing block */
		}						/* end while, fall through to next table */

		/* No more tuples in this edge table, try next table */
		(*start_table_idx)++;
		hop_close_edge_scan(hop);
	}							/* end for(;;) */
}

/*
 * Store the PK values of a same-var group's first occurrence so later
 * occurrences can be compared against it.
 */
static void
store_samevar_keys(GraphScanState * node, int gid, int nkeys,
				   const Datum *values, const bool *nulls)
{
	node->samevar_nkeys[gid] = nkeys;
	if (nkeys > 0)
		set_datum_pair(nkeys, &node->samevar_key_values[gid],
					   &node->samevar_key_nulls[gid],
					   values, nulls);
}

/*
 * For a same-var group whose first occurrence is pi=0, store the start
 * vertex's PK values when we fetch a new start vertex.  Called from
 * start_vertex_advance after a start vertex is accepted.
 */
static void
store_start_vertex_identity(GraphScanState * node)
{
	int			pi = 0;
	int			gid;

	if (pi >= node->num_pi)
		return;
	gid = node->samevar_group[pi];
	if (gid < 0)
		return;
	if (node->samevar_elem_kind[gid] != 'v')
		return;

	/* Store PK values from the start vertex */
	store_samevar_keys(node, gid, node->n_start_keys,
					   node->start_vertex_values,
					   node->start_vertex_nulls);
}

/*
 * Check whether the edge at edge_elem_idx must match a previously-occurring
 * edge with the same variable name.  Returns false if the edge should be
 * rejected (identity mismatch).
 */
static bool
check_samevar_edge_identity(GraphScanState * node, int edge_elem_idx,
							GraphHopFrame * hop)
{
	int			gid;
	int			first_pi;

	if (edge_elem_idx < 0 || edge_elem_idx >= node->num_pi)
		return true;
	gid = node->samevar_group[edge_elem_idx];
	if (gid < 0)
		return true;
	if (node->samevar_elem_kind[gid] != 'e')
		return true;

	first_pi = node->samevar_first_pi[gid];

	if (edge_elem_idx == first_pi)
	{
		/* First occurrence: store TID for future comparisons */
		if (hop->edge_slot)
			ItemPointerCopy(&hop->edge_slot->tts_tid,
							&node->samevar_tid[gid]);
		return true;
	}
	else
	{
		/* Compare TID */
		if (!hop->edge_slot)
			return false;
		return ItemPointerEquals(&hop->edge_slot->tts_tid,
								 &node->samevar_tid[gid]);
	}
}

/*
 * Check whether the destination vertex at dest_elem_idx must match a
 * previously-occurring vertex with the same variable name.  Returns false
 * if the vertex should be rejected (identity mismatch).
 *
 * If this is the first occurrence (not pi=0, handled separately), stores
 * the PK values for future comparisons.
 */
static bool
check_samevar_vertex_identity(GraphScanState * node, int dest_elem_idx,
							  GraphHopFrame * hop,
							  GraphElementRelInfo * dest_info)
{
	int			gid;
	int			first_pi;

	if (dest_elem_idx < 0 || dest_elem_idx >= node->num_pi)
		return true;
	gid = node->samevar_group[dest_elem_idx];
	if (gid < 0)
		return true;
	if (node->samevar_elem_kind[gid] != 'v')
		return true;

	first_pi = node->samevar_first_pi[gid];

	if (dest_elem_idx == first_pi && first_pi != 0)
	{
		/*
		 * First occurrence at a non-zero position: store PK values from the
		 * hop's to_vertex for future comparisons.
		 */
		store_samevar_keys(node, gid, hop->nto,
						   hop->to_vertex_values, hop->to_vertex_nulls);
		return true;
	}
	else
	{
		/* Subsequent occurrence: compare against stored reference */
		int			nkeys = node->samevar_nkeys[gid];
		FmgrInfo   *key_eq = (dest_info != NULL) ? dest_info->key_eq : NULL;
		int			maxkeys = (dest_info != NULL) ?
			dest_info->nkey_attnums : 0;
		int			ki;

		for (ki = 0; ki < nkeys; ki++)
		{
			bool		ref_null;

			if (ki >= hop->nto || ki >= maxkeys)
				return false;

			ref_null = (node->samevar_key_nulls[gid] ?
						node->samevar_key_nulls[gid][ki] : false);
			if (hop->to_vertex_nulls[ki] != ref_null)
				return false;
			if (!hop->to_vertex_nulls[ki] &&
				(key_eq == NULL ||
				 !graph_eq_datum(&key_eq[ki],
								 hop->to_vertex_values[ki], false,
								 node->samevar_key_values[gid][ki], false)))
				return false;
		}
		return true;
	}
}

/* ----------------------------------------------------------------
 *	 hop_close_edge_scan
 * ----------------------------------------------------------------
 */
static void
hop_close_edge_scan(GraphHopFrame * hop)
{
	if (hop->edge_scan)
	{
		table_endscan(hop->edge_scan);
		hop->edge_scan = NULL;
	}
	if (hop->edge_slot)
	{
		ExecDropSingleTupleTableSlot(hop->edge_slot);
		hop->edge_slot = NULL;
	}
}

/*
 * Release a VLE stack frame's storage (key values, scan, slot).
 */
static void
vle_free_frame(GraphVLEFrame * fr)
{
	if (fr->edge_scan)
	{
		table_endscan(fr->edge_scan);
		fr->edge_scan = NULL;
	}
	if (fr->edge_slot)
	{
		ExecDropSingleTupleTableSlot(fr->edge_slot);
		fr->edge_slot = NULL;
	}
	if (fr->values)
		pfree(fr->values);
	if (fr->nulls)
		pfree(fr->nulls);
	fr->values = NULL;
	fr->nulls = NULL;
}

/*
 * Mark a VLE hop as inactive, resetting all its scalar DFS state.
 */
static void
vle_deactivate(GraphHopFrame * hop)
{
	hop->vle_active = false;
	hop->vle_lower = 0;
	hop->vle_upper = 0;
	hop->vle_stack_top = -1;
	hop->vle_stack_capacity = 0;
}

/* ----------------------------------------------------------------
 *	 hop_reset
 * ----------------------------------------------------------------
 */
static void
hop_reset(GraphHopFrame * hop)
{
	hop_close_edge_scan(hop);
	if (hop->to_vertex_slot)
	{
		ExecDropSingleTupleTableSlot(hop->to_vertex_slot);
		hop->to_vertex_slot = NULL;
	}
	free_datum_pair(&hop->to_vertex_values, &hop->to_vertex_nulls);
	hop->nto = 0;
	free_datum_pair(&hop->from_vertex_values, &hop->from_vertex_nulls);
	hop->nfrom = 0;

	/*
	 * Don't drop from_vertex_slot. It is owned by the previous hop or by the
	 * start vertex state.
	 */
	hop->from_vertex_slot = NULL;
	hop->from_vertex_elemid = InvalidOid;
	hop->undirected_has_seen = false;
	hop->edge_counted = false;
	/* VLE stack fields managed by hop_advance / hop_reset */
	if (hop->vle_stack)
	{
		for (int vi = 0; vi < hop->vle_stack_capacity; vi++)
			vle_free_frame(&hop->vle_stack[vi]);
		pfree(hop->vle_stack);
		hop->vle_stack = NULL;
	}
	vle_deactivate(hop);
}

/*
 * Initialize a GraphHopFrame with the given source vertex context.
 * All edge-scan state is reset to 0.  The from_vertex_slot is borrowed
 * (caller owns it), not copied.
 */
static void
hop_init(GraphHopFrame * hop, int nfrom, Datum *from_values,
		 bool *from_nulls, TupleTableSlot *from_vertex_slot,
		 Oid from_vertex_elemid)
{
	hop->nfrom = nfrom;
	set_datum_pair(nfrom, &hop->from_vertex_values,
				   &hop->from_vertex_nulls, from_values, from_nulls);
	hop->from_vertex_slot = from_vertex_slot;
	hop->from_vertex_elemid = from_vertex_elemid;
	hop_close_edge_scan(hop);
	hop->edge_table_index = 0;
	hop->scan_pass = 0;

	/*
	 * Don't drop to_vertex_slot here. It may be reused as from_vertex_slot by
	 * the caller (e.g., VLE re-init).
	 */
	hop->undirected_has_seen = false;
	hop->edge_counted = false;

	/*
	 * VLE stack fields are NOT reset here. They are managed by hop_advance
	 * and hop_reset.
	 */
}

/* ----------------------------------------------------------------
 *	 ExtractTargetVertexKeys
 *
 *		Extract the target vertex PK column values from an edge tuple.
 * ----------------------------------------------------------------
 */
static void
ExtractTargetVertexKeys(TupleTableSlot *edge_slot,
						GraphElementRelInfo * edge_info,
						EdgeDirection dir, int scan_pass,
						int nkeys, Datum *values, bool *nulls)
{
	int			ki;
	AttrNumber *attnums;
	int			n_fk;

	if (edge_is_incoming(dir, scan_pass))
	{
		n_fk = edge_info->nsrc_fk_attnums;
		attnums = edge_info->src_fk_attnums;
	}
	else
	{
		n_fk = edge_info->ndst_fk_attnums;
		attnums = edge_info->dst_fk_attnums;
	}

	if (nkeys != n_fk)
		return;

	for (ki = 0; ki < n_fk; ki++)
		values[ki] = slot_getattr(edge_slot, attnums[ki], &nulls[ki]);
}

/*
 * Find the element index of the vertex element with the given OID, or -1
 * if no vertex element matches.  Used to locate the source or destination
 * vertex element of an edge.
 */
static int
find_vertex_for_edge(GraphScanState * node, Oid vertex_elemid)
{
	int			i;

	if (!OidIsValid(vertex_elemid))
		return -1;

	for (i = 0; i < node->num_elements; i++)
	{
		if (node->element_info[i].element_kind != 'v')
			continue;
		if (node->element_info[i].element_oid == vertex_elemid)
			return i;
	}
	return -1;
}

/*
 * fetch_vertex_by_pk
 *
 *		Look up a vertex tuple by primary key values. Uses a sequential scan
 *		of the vertex table.
 *		Returns a materialized TupleTableSlot, or NULL if not found.
 *		Caller is responsible for freeing the slot.
 */
static TupleTableSlot *
fetch_vertex_by_pk(GraphElementRelInfo * vinfo, Datum *pk_values,
				   bool *pk_nulls, int npk,
				   EState *estate)
{
	TupleTableSlot *slot;

	if (npk == 0 || vinfo->nkey_attnums != npk)
		return NULL;

	/*
	 * For partitioned/inherited, create a virtual slot to avoid TupleDesc
	 * matching issues with ExecCopySlot.  For all other types, use the
	 * element's slot type.
	 */
	if (is_partitioned_or_inherited(vinfo))
		slot = MakeSingleTupleTableSlot(vinfo->tupdesc, &TTSOpsVirtual);
	else
		slot = element_make_slot(vinfo);

	/*
	 * For plain relations, use an independent one-off heap scan.  This must
	 * NOT use the shared element scan state (info->scan.scan): the same
	 * physical element can appear at multiple pattern positions (e.g.
	 * (a)->(b)->(a)), in which case the start-vertex scan and a hop's
	 * destination fetch would clobber each other's scan handle.
	 */
	if (is_plain_relation(vinfo))
	{
		TableScanDesc scan;

		scan = table_beginscan(vinfo->heap_rel, estate->es_snapshot,
							   0, NULL, SO_NONE);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			if (slot_matches_values(slot, npk, vinfo->key_attnums,
									vinfo->key_eq, pk_values, pk_nulls))
			{
				ExecMaterializeSlot(slot);
				table_endscan(scan);
				return slot;
			}
		}
		table_endscan(scan);
		ExecDropSingleTupleTableSlot(slot);
		return NULL;
	}

	/*
	 * For partitioned/inherited tables: scan children directly, using our own
	 * child iteration state so we don't interfere with the shared element's
	 * current_child and child_scans.
	 */
	if (is_partitioned_or_inherited(vinfo))
	{
		for (int ci = 0; ci < vinfo->scan.nchildren; ci++)
		{
			TableScanDesc child_scan;

			child_scan = table_beginscan(vinfo->scan.child_rels[ci],
										 estate->es_snapshot,
										 0, NULL, SO_NONE);
			while (table_scan_getnextslot(child_scan,
										  ForwardScanDirection,
										  vinfo->scan.child_slots[ci]))
			{
				if (slot_matches_values(vinfo->scan.child_slots[ci], npk,
										vinfo->key_attnums, vinfo->key_eq,
										pk_values, pk_nulls))
				{
					ExecMaterializeSlot(vinfo->scan.child_slots[ci]);
					table_endscan(child_scan);
					copy_child_slot(slot, vinfo->scan.child_slots[ci]);
					ExecMaterializeSlot(slot);
					return slot;
				}
			}
			table_endscan(child_scan);
		}
		ExecDropSingleTupleTableSlot(slot);
		return NULL;
	}

	/*
	 * For views and FDW, scan via element_scan_next and filter by PK locally.
	 * These don't share state with the start vertex scan.
	 */
	element_scan_start(vinfo, estate->es_snapshot);
	while (element_scan_next(vinfo, slot))
	{
		if (slot_matches_values(slot, npk, vinfo->key_attnums,
								vinfo->key_eq, pk_values, pk_nulls))
		{
			ExecMaterializeSlot(slot);
			element_scan_end(vinfo);
			return slot;
		}
	}
	element_scan_end(vinfo);
	ExecDropSingleTupleTableSlot(slot);
	return NULL;
}

/*
 * Fill one attribute of a tuple slot from the column mapping.
 * ci is the column mapping index, slot_att is the 0-based attribute
 * position in the target slot.  Reads from the appropriate element slot via
 * slot_for_elem cache.
 */
static void
fill_one_slot_attr(GraphScanState * node, TupleTableSlot *slot,
				   int slot_att, int ci)
{
	int			pi = node->col_elem_index[ci];

	if (pi >= 0 && pi < node->num_pi &&
		node->slot_for_elem[pi] != NULL)
	{
		TupleTableSlot *elem_slot = node->slot_for_elem[pi];
		bool		isnull = false;

		if (node->col_propid[ci] != InvalidOid &&
			node->col_prop_unsafe[ci])
		{
			/* label-unsafe property: always resolve via the expression */
			GraphElementPattern *gep = GetPatternElement(node, pi);

			slot->tts_values[slot_att] =
				resolve_graph_property(node, node->col_propid[ci],
									   elem_slot, &isnull,
									   gep && IS_EDGE_PATTERN(gep->kind));
			slot->tts_isnull[slot_att] = isnull;
		}
		else if (fill_physical_prop_value(node, elem_slot,
										  node->col_propid[ci],
										  node->col_propname[ci], false,
										  &slot->tts_values[slot_att],
										  &isnull))
		{
			/* direct physical-column read */
			slot->tts_isnull[slot_att] = isnull;
		}
		else if (node->col_propid[ci] != InvalidOid)
		{
			/* valid property with no physical column: resolve at runtime */
			GraphElementPattern *gep = GetPatternElement(node, pi);

			slot->tts_values[slot_att] =
				resolve_graph_property(node, node->col_propid[ci],
									   elem_slot, &isnull,
									   gep && IS_EDGE_PATTERN(gep->kind));
			slot->tts_isnull[slot_att] = isnull;
		}
		else
		{
			/* name-only property with no matching column: NULL */
			slot->tts_values[slot_att] = (Datum) 0;
			slot->tts_isnull[slot_att] = true;
		}
	}
	else
	{
		slot->tts_values[slot_att] = (Datum) 0;
		slot->tts_isnull[slot_att] = true;
	}
}

/*
 * MaterializeCurrentPathSlot
 *
 * Fill the result tuple slot with values from the active path,
 * using the column mapping built during ExecInitGraphScan.
 * Returns the slot.
 */
static TupleTableSlot *
MaterializeCurrentPathSlot(GraphScanState * node)
{
	update_slot_cache(node);

	if (node->ss.ps.ps_ProjInfo != NULL)
	{
		TupleTableSlot *scan_slot = node->ss.ss_ScanTupleSlot;
		int			scan_natts = scan_slot->tts_tupleDescriptor->natts;
		int			ci;

		ExecClearTuple(scan_slot);

		/* Initialize all scan slot values to NULL (clear stale data) */
		init_slot_values_null(scan_slot, scan_natts);

		/*
		 * Populate ALL scan slot attributes from the column mapping.
		 * ExecProject's compiled steps use EEOP_SCAN_VAR to read from the
		 * scan slot, so every attribute must be filled regardless of whether
		 * the outer query's target list references it as a bare Var or wraps
		 * it in a FuncExpr/RelabelType/etc.
		 */
		for (ci = 0; ci < scan_natts; ci++)
			fill_one_slot_attr(node, scan_slot, ci, ci);

		/*
		 * For columns that have a compiled expression (e.g., FuncExpr
		 * wrapping a GraphPropertyRef), evaluate the expression with the
		 * element slot as ecxt_outertuple and store the result into the scan
		 * slot, overriding the raw property value.
		 */
		{
			ExprContext *econtext = node->ss.ps.ps_ExprContext;

			for (ci = 0; ci < scan_natts; ci++)
			{
				if (node->col_exprs &&
					ci < node->ncols &&
					node->col_exprs[ci] != NULL)
				{
					int			pi = node->col_expr_elem_index[ci];
					TupleTableSlot *elem_slot = NULL;

					if (pi >= 0 && pi < node->num_pi)
						elem_slot = node->slot_for_elem[pi];

					/*
					 * Evaluate the column expression.  Constant columns have
					 * no element dependency, so they are evaluated even when
					 * no element slot is available.
					 */
					if (elem_slot || pi < 0)
					{
						if (elem_slot)
							econtext->ecxt_outertuple = elem_slot;
						scan_slot->tts_values[ci] =
							ExecEvalExpr(node->col_exprs[ci],
										 econtext,
										 &scan_slot->tts_isnull[ci]);
					}
				}
			}
		}

		scan_slot->tts_nvalid = scan_natts;
		ExecStoreVirtualTuple(scan_slot);

		{
			ExprContext *econtext = node->ss.ps.ps_ExprContext;

			econtext->ecxt_scantuple = scan_slot;
			return ExecProject(node->ss.ps.ps_ProjInfo);
		}
	}
	else
	{
		TupleTableSlot *slot = node->ss.ps.ps_ResultTupleSlot;
		int			ci;
		int			ncols;

		/*
		 * Use the scan tuple descriptor's column count, not the plan target
		 * list's.  When the GraphScan is inside a subquery, the outer plan
		 * may pass an empty target list (e.g., for count(*)), but the scan
		 * still needs to materialize its full column set so that ExecQual can
		 * read Var(varno=scanrelid) references.
		 */
		ncols = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor ?
			node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts : 0;

		ExecClearTuple(slot);

		for (ci = 0; ci < ncols; ci++)
			fill_one_slot_attr(node, slot, ci, ci);

		slot->tts_nvalid = ncols;
		ExecStoreVirtualTuple(slot);
		return slot;
	}
}

/* ----------------------------------------------------------------
 *	 vle_push_frame
 *
 *		Push a vertex state onto the VLE DFS stack at the next depth.
 *		The key values are copied into palloc'd storage owned by the
 *		stack frame.  Does NOT modify edge_table_index/scan_pass in
 *		the frame (those are set when the frame's edge scan begins).
 * ----------------------------------------------------------------
 */
static void
vle_push_frame(GraphHopFrame * hop, Oid elem_oid, int nkeys,
			   Datum *values, bool *nulls)
{
	int			top = hop->vle_stack_top + 1;
	GraphVLEFrame *fr;

	Assert(top < hop->vle_stack_capacity);

	fr = &hop->vle_stack[top];
	fr->elemid = elem_oid;
	fr->nkeys = nkeys;
	set_datum_pair(nkeys, &fr->values, &fr->nulls, values, nulls);
	/* Resume position: start fresh scan from this vertex */
	fr->table_idx = 0;
	fr->scan_pass = 0;
	fr->edge_scan = NULL;
	fr->edge_slot = NULL;

	hop->vle_stack_top = top;
}

/* ----------------------------------------------------------------
 *	 vle_pop_frame
 *
 *		Pop the top frame from the VLE DFS stack, freeing its storage.
 *		Frames at depth >= 1 represent a traversed edge, so they release
 *		one unit from the running total path-length counter (depth 0 is
 *		the source vertex and carries no edge).
 * ----------------------------------------------------------------
 */
static void
vle_pop_frame(GraphScanState * node, GraphHopFrame * hop)
{
	int			top = hop->vle_stack_top;
	GraphVLEFrame *fr;

	Assert(top >= 0);
	if (top >= 1)
		node->path_len--;
	fr = &hop->vle_stack[top];
	vle_free_frame(fr);
	hop->vle_stack_top--;
}

/* ----------------------------------------------------------------
 *	 vle_activate
 *
 *		Allocate and initialize the VLE DFS stack for the given hop,
 *		and push the hop's current from_vertex as depth 0.
 * ----------------------------------------------------------------
 */
static void
vle_activate(GraphScanState * node, GraphHopFrame * hop,
			 GraphElementPattern *edge_elem)
{
	List	   *quant = edge_elem->quantifier;
	int			lower;
	int			upper;
	int			capacity;

	Assert(list_length(quant) == 2);
	lower = linitial_int(quant);
	upper = lsecond_int(quant);
	if (upper == -1)
	{
		/*
		 * Unbounded quantifier: allow one edge past max_graph_stack_depth so
		 * the total path-length guard in hop_advance can raise the depth
		 * error for runaway (e.g. cyclic) traversals, instead of silently
		 * clamping the recursion here.
		 */
		upper = max_graph_stack_depth + 1;
	}

	capacity = upper + 1;

	hop->vle_lower = lower;
	hop->vle_upper = upper;
	hop->vle_stack_capacity = capacity;
	hop->vle_stack = (GraphVLEFrame *)
		palloc0(sizeof(GraphVLEFrame) * capacity);
	hop->vle_stack_top = -1;

	/* Push the current source vertex at depth 0 */
	vle_push_frame(hop, hop->from_vertex_elemid,
				   hop->nfrom, hop->from_vertex_values,
				   hop->from_vertex_nulls);

	hop->vle_active = true;
}

/*
 * Raise the depth error when a path would exceed max_graph_stack_depth
 * (the maximum total path length for the whole graph traversal).
 */
static void
graph_depth_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("exceeded maximum graph traversal depth"),
			 errhint("The query may contain a cyclic or excessively long path "
					 "pattern. Try simplifying the pattern or increasing "
					 "max_graph_stack_depth.")));
}

/*
 * Charge one traversed edge to the current path's running length, raising
 * the depth error when it would exceed max_graph_stack_depth.
 */
static void
graph_charge_edge(GraphScanState * node)
{
	if (++node->path_len > max_graph_stack_depth)
		graph_depth_error();
}

/* ----------------------------------------------------------------
 *	 hop_advance
 *
 *		Try to advance the DFS chain from hop hi by finding the next
 *		matching edge for that hop.  Returns true if the chain made
 *		progress (a new path segment was found), false if the current
 *		hop is exhausted and we must backtrack.
 *
 *		On success, if hi is the last hop, *path_complete is set to
 *		true and the result path is materialized in the scan slot.
 *		If there are more hops ahead, the next hop is initialized.
 *
 *		For VLE hops, this function implements a DFS-with-stack:
 *		- When vle_active is false, first-call logic activates VLE.
 *		- When vle_active is true, the stack guides DFS.
 *		- Returns false only when the entire stack is exhausted.
 * ----------------------------------------------------------------
 */
static bool
hop_advance(GraphScanState * node, int hi, bool *path_complete,
			bool *vle_continue)
{
	GraphHopFrame *hop = &node->hops[hi];
	int			edge_elem_idx = 2 * hi + 1;
	GraphElementPattern *edge_elem = GetPatternElement(node, edge_elem_idx);
	EdgeDirection dir = GRAPH_DIR_OUTGOING;
	bool		is_vle_hop = false;

	*path_complete = false;
	*vle_continue = false;

	/* Determine direction from the pattern element */
	if (edge_elem)
	{
		switch (edge_elem->kind)
		{
			case EDGE_PATTERN_LEFT:
				dir = GRAPH_DIR_INCOMING;
				break;
			case EDGE_PATTERN_ANY:
				dir = GRAPH_DIR_UNDIRECTED;
				break;
			default:
				dir = GRAPH_DIR_OUTGOING;
				break;
		}
	}

	/*
	 * Determine if this is a VLE hop (has a quantifier other than the
	 * implicit {1,1}).  Any hop may carry a quantifier; intermediate VLE hops
	 * hand each matched destination to the following hop instead of
	 * materializing a complete path.
	 */
	if (edge_elem && edge_elem->quantifier != NULL &&
		(linitial_int(edge_elem->quantifier) != 1 ||
		 lsecond_int(edge_elem->quantifier) != 1))
		is_vle_hop = true;

	/* --- Path 1: Not VLE (implicit {1,1}, or intermediate hop) --- */
	if (!is_vle_hop && !hop->vle_active)
	{
		if (!vle_find_next_edge(node,
								hop->from_vertex_elemid,
								hop->nfrom, hop->from_vertex_values,
								hop->from_vertex_nulls,
								&hop->edge_table_index, &hop->scan_pass,
								edge_elem_idx, dir,
								hop,
								&hop->to_vertex_elemid,
								&hop->nto, &hop->to_vertex_values,
								&hop->to_vertex_nulls))
			return false;

		/* Found a matching (edge, dest_vertex) */

		/*
		 * Charge the fixed hop's single edge to the running path length on
		 * its first resolution; sibling edges replace it, so subsequent
		 * successes for the same hop must not double count.
		 */
		if (!hop->edge_counted)
		{
			hop->edge_counted = true;
			graph_charge_edge(node);
		}

		if (hi + 1 < node->n_hops)
		{
			/* More hops ahead, initialize the next one */
			hop_init(&node->hops[hi + 1], hop->nto,
					 hop->to_vertex_values, hop->to_vertex_nulls,
					 hop->to_vertex_slot, hop->to_vertex_elemid);
			return true;
		}

		/* Last hop / path is complete */
		*path_complete = true;
		return true;
	}

	/* --- Path 2: VLE hop --- */

	/* First call for this VLE hop: activate the stack */
	if (!hop->vle_active)
	{
		/* Push the current source vertex at depth 0 and activate */
		vle_activate(node, hop, edge_elem);

		/*
		 * For {0,N}: depth 0 itself is a valid result (zero hops). Yield the
		 * start vertex before scanning any edges.
		 */
		if (0 >= hop->vle_lower)
		{
			if (hi + 1 < node->n_hops)
			{
				/*
				 * Intermediate {0,N} hop: hand the source vertex itself to
				 * the following hop as the zero-hop result.
				 */
				hop_init(&node->hops[hi + 1],
						 hop->nfrom, hop->from_vertex_values,
						 hop->from_vertex_nulls,
						 hop->from_vertex_slot,
						 hop->from_vertex_elemid);
				*vle_continue = false;
			}
			else
			{
				*path_complete = true;
				*vle_continue = (0 < hop->vle_upper);
			}
			return true;
		}
	}

	/*
	 * DFS loop: scan edges from the current frame's source vertex. We loop on
	 * backtrack: when a frame is exhausted, pop and try the parent frame's
	 * next sibling edge.
	 *
	 * Each frame owns its own edge_scan/edge_slot, kept alive in the frame's
	 * GraphVLEFrame.  We swap them into hop->edge_scan/edge_slot before
	 * calling vle_find_next_edge (which operates on the hop-level fields),
	 * then swap back so the frame retains ownership.
	 */
	for (;;)
	{
		int			frame_idx = hop->vle_stack_top;

		Assert(frame_idx >= 0);

		/*
		 * If this frame is already at max depth (>= vle_upper), it cannot go
		 * deeper.  Pop it and continue backtracking.
		 */
		if (frame_idx >= hop->vle_upper)
		{
			vle_pop_frame(node, hop);
			if (hop->vle_stack_top < 0)
			{
				vle_deactivate(hop);
				return false;
			}
			continue;
		}

		/*
		 * Set the hop's from_vertex to this frame's vertex so
		 * vle_find_next_edge knows the source.
		 */
		{
			GraphVLEFrame *fr = &hop->vle_stack[frame_idx];
			int			f_nkeys = fr->nkeys;

			if (hop->nfrom != f_nkeys)
			{
				if (hop->from_vertex_values)
					pfree(hop->from_vertex_values);
				if (hop->from_vertex_nulls)
					pfree(hop->from_vertex_nulls);
				hop->from_vertex_values =
					(Datum *) palloc(sizeof(Datum) * f_nkeys);
				hop->from_vertex_nulls =
					(bool *) palloc(sizeof(bool) * f_nkeys);
				hop->nfrom = f_nkeys;
			}
			memcpy(hop->from_vertex_values,
				   fr->values,
				   sizeof(Datum) * f_nkeys);
			memcpy(hop->from_vertex_nulls,
				   fr->nulls,
				   sizeof(bool) * f_nkeys);
			hop->from_vertex_elemid = fr->elemid;
		}

		/*
		 * Swap in this frame's own edge scan and restore its resume position.
		 * Since each frame owns its own scan, an in-progress scan continues
		 * exactly where it left off.
		 */
		hop->edge_scan = hop->vle_stack[frame_idx].edge_scan;
		hop->edge_slot = hop->vle_stack[frame_idx].edge_slot;
		hop->edge_table_index = hop->vle_stack[frame_idx].table_idx;
		hop->scan_pass = hop->vle_stack[frame_idx].scan_pass;

		/*
		 * Find the next edge from the current source vertex, using the
		 * frame's scan (or starting fresh if NULL).
		 */
		if (vle_find_next_edge(node,
							   hop->from_vertex_elemid,
							   hop->nfrom, hop->from_vertex_values,
							   hop->from_vertex_nulls,
							   &hop->edge_table_index, &hop->scan_pass,
							   edge_elem_idx, dir,
							   hop,
							   &hop->to_vertex_elemid,
							   &hop->nto, &hop->to_vertex_values,
							   &hop->to_vertex_nulls))
		{
			int			new_depth;
			bool		result_found;
			bool		continue_deeper;

			/*
			 * Found an edge.  Swap back: the frame keeps ownership of its
			 * scan (now positioned after this edge).
			 */
			hop->vle_stack[frame_idx].edge_scan = hop->edge_scan;
			hop->vle_stack[frame_idx].edge_slot = hop->edge_slot;
			hop->vle_stack[frame_idx].table_idx = hop->edge_table_index;
			hop->vle_stack[frame_idx].scan_pass = hop->scan_pass;
			hop->edge_scan = NULL;
			hop->edge_slot = NULL;

			/*
			 * Push the destination vertex onto the stack at the next depth
			 * level.
			 */
			vle_push_frame(hop, hop->to_vertex_elemid,
						   hop->nto, hop->to_vertex_values,
						   hop->to_vertex_nulls);

			/*
			 * Each stack frame at depth >= 1 corresponds to one traversed
			 * edge, so extend the running path length and enforce the total
			 * traversal-depth limit.
			 */
			graph_charge_edge(node);

			new_depth = hop->vle_stack_top;

			/* Determine if this depth is a valid result */
			result_found = (new_depth >= hop->vle_lower);
			continue_deeper = (new_depth < hop->vle_upper);

			if (result_found)
			{
				if (hi + 1 < node->n_hops)
				{
					/*
					 * Intermediate VLE hop: hand the matched destination
					 * vertex to the following hop.  Do NOT set
					 * *path_complete; the caller will instead advance to the
					 * next hop (vle_continue stays false below).  The VLE
					 * stack remains live so that once the following hop is
					 * exhausted and we backtrack to this one, its sibling
					 * edges can still be explored.
					 */
					hop_init(&node->hops[hi + 1],
							 hop->nto, hop->to_vertex_values,
							 hop->to_vertex_nulls,
							 hop->to_vertex_slot,
							 hop->to_vertex_elemid);
					*vle_continue = false;
				}
				else
				{
					/* Last VLE hop: this is a complete path */
					*path_complete = true;
				}
			}

			if (continue_deeper)
			{
				/*
				 * Keep exploring deeper from this destination vertex. Its
				 * scan is NULL (vle_push_frame zeros it), so the next call
				 * will open a fresh scan.
				 *
				 * If this hop yielded no result yet (depth below lower
				 * bound), stay on this hop to keep drilling; if it did yield
				 * a result we have already signalled the caller
				 * (path_complete for the last hop, or advance for an
				 * intermediate hop) and the frame keeps state for later.
				 */
				if (!result_found)
					*vle_continue = true;
			}
			else
			{
				/*
				 * At max depth: pop this frame since we won't explore deeper.
				 * Its vertex may have been yielded (if within lower bound).
				 * vle_pop_frame closes/frees its scan. If parent frames
				 * remain, signal the caller to continue DFS for sibling edges
				 * -- unless this is an intermediate hop that already advanced
				 * to the next hop.
				 */
				vle_pop_frame(node, hop);
				if (hop->vle_stack_top >= 0 && !result_found)
					*vle_continue = true;
			}

			return true;
		}

		/*
		 * No more edges from the current source vertex.  Swap back (the scan
		 * may have been closed by vle_find_next_edge during table
		 * exhaustion), then pop this frame.
		 */
		hop->vle_stack[frame_idx].edge_scan = hop->edge_scan;
		hop->vle_stack[frame_idx].edge_slot = hop->edge_slot;
		hop->edge_scan = NULL;
		hop->edge_slot = NULL;

		vle_pop_frame(node, hop);

		if (hop->vle_stack_top < 0)
		{
			/* Entire stack exhausted. VLE hop is done */
			vle_deactivate(hop);
			return false;
		}

		/* Back to parent frame. The loop will swap in its scan next iteration */
	}

	/* NOTREACHED */
	return false;
}

/* ----------------------------------------------------------------
 *	 hop_backtrack
 *
 *		Clean up the hop at index hi and return to the previous hop.
 *		If the previous hop exists, its to_vertex_slot is released
 *		(that destination is no longer reachable).
 *		Returns the new hop index (-1 if no more hops to backtrack to).
 * ----------------------------------------------------------------
 */
static int
hop_backtrack(GraphScanState * node, int hi)
{
	/* Releasing a fixed hop removes its single edge from the path length */
	if (node->hops[hi].edge_counted)
	{
		node->hops[hi].edge_counted = false;
		node->path_len--;
	}
	hop_reset(&node->hops[hi]);
	hi--;

	if (hi >= 0)
	{
		/* Release the stale destination of the previous hop */
		GraphHopFrame *prev = &node->hops[hi];

		if (prev->to_vertex_slot)
		{
			ExecDropSingleTupleTableSlot(prev->to_vertex_slot);
			prev->to_vertex_slot = NULL;
		}
	}
	return hi;
}

/* ----------------------------------------------------------------
 *	 ExecGraphScan
 *
 *		Core hop-based DFS state machine.
 *
 *		State machine phases:
 *		  PHASE_START_VERTEX:	fetch the next start vertex
 *		  PHASE_EDGE_SCAN:		try to advance the current hop chain;
 *								on success, if path is complete, return it;
 *								on exhaustion, backtrack or fetch next start
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecGraphScan(PlanState *pstate)
{
	GraphScanState *node = castNode(GraphScanState, pstate);
	int			hi;

	if (!node->is_initialized)
	{
		init_start_vertex(node);
		node->is_initialized = true;
	}

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();

		/* --- Phase: start vertex --- */
		if (node->current_hop == -1)
		{
			/* Fetch next start vertex; return NULL when all are exhausted */
			if (!start_vertex_advance(node))
				return NULL;

			/*
			 * A new start vertex begins a fresh DFS effort: the running total
			 * path length starts over for its paths.
			 */
			node->path_len = 0;

			node->slot_for_elem[0] = node->start_vertex_slot;

			/* Vertex-only pattern: materialize and return */
			if (node->n_hops == 0)
			{
				TupleTableSlot *result = MaterializeCurrentPathSlot(node);

				if (result &&
					evaluate_graph_where(node))
				{
					ExprContext *econtext = node->ss.ps.ps_ExprContext;

					/*
					 * Use the scan slot for ecxt_scantuple, not the result
					 * slot.  MaterializeCurrentPathSlot may return
					 * ps_ResultTupleSlot (via ExecProject) which could have
					 * fewer columns than the scan slot when the outer plan
					 * doesn't need all graph columns (e.g., count(*)
					 * subquery). Var(scanrelid, attno) in plan->qual reads
					 * from ecxt_scantuple.
					 */
					econtext->ecxt_scantuple = node->ss.ss_ScanTupleSlot;

					if (ExecQual(node->ss.ps.qual, econtext))
						return result;
				}
				continue;
			}

			/* Initialize hop 0 with the new start vertex */
			{
				GraphElementPattern *elem0 = GetPatternElement(node, 0);
				GraphElementRelInfo *rel0 =
					GetElementRelInfo(node, elem0, node->start_table_index);

				hop_init(&node->hops[0], node->n_start_keys,
						 node->start_vertex_values,
						 node->start_vertex_nulls,
						 node->start_vertex_slot,
						 rel0 ? rel0->element_oid : InvalidOid);
			}

			node->current_hop = 0;
		}

		/* --- Phase: DFS edge scan / backtrack --- */
		hi = node->current_hop;

		while (hi >= 0 && hi < node->n_hops)
		{
			bool		path_complete;
			bool		vle_continue;

			if (hop_advance(node, hi, &path_complete, &vle_continue))
			{
				if (path_complete)
				{
					TupleTableSlot *result;

					/* Complete path found, materialize */
					node->current_hop = hi;
					result = MaterializeCurrentPathSlot(node);

					/* Check the outer GraphPattern WHERE clause */
					if (!evaluate_graph_where(node))
						continue;

					/*
					 * Check the plan-level qual (outer Var refs from LATERAL
					 * joins, pushed-down quals, etc.)
					 */
					{
						ExprContext *econtext = node->ss.ps.ps_ExprContext;

						econtext->ecxt_scantuple =
							node->ss.ss_ScanTupleSlot;
						if (!ExecQual(node->ss.ps.qual, econtext))
							continue;
					}

					return result;
				}

				if (!vle_continue)
				{
					/* Advance to the next hop in the chain */
					hi++;
					node->current_hop = hi;
				}

				/*
				 * If vle_continue is true, stay on the same hop. The VLE
				 * stack in hop_advance handles continuation.
				 */
			}
			else
			{
				/* Current hop exhausted, backtrack */
				hi = hop_backtrack(node, hi);
				node->current_hop = hi;
			}
		}

		/* All hops exhausted for the current start vertex */
		{
			int			i;

			for (i = 0; i < node->n_hops; i++)
				hop_reset(&node->hops[i]);
		}
		node->current_hop = -1;
	}
}

/* ----------------------------------------------------------------
 *	 ExecEndGraphScan
 * ----------------------------------------------------------------
 */
void
ExecEndGraphScan(GraphScanState * node)
{
	int			i;

	element_info_cleanup(node);

	if (node->start_vertex_slot)
	{
		ExecDropSingleTupleTableSlot(node->start_vertex_slot);
		node->start_vertex_slot = NULL;
	}

	if (node->hops)
	{
		for (i = 0; i < node->n_hops; i++)
			hop_reset(&node->hops[i]);
	}

	/* Drop virtual qual slots owned by graph_cxt before deleting it */
	if (node->qual_virt_slot)
	{
		ExecDropSingleTupleTableSlot(node->qual_virt_slot);
		node->qual_virt_slot = NULL;
	}
	if (node->element_qual_slots)
	{
		for (i = 0; i < node->num_pi; i++)
		{
			if (node->element_qual_slots[i])
				ExecDropSingleTupleTableSlot(node->element_qual_slots[i]);
		}
	}

	/*
	 * All per-query arrays (column mapping, compiled quals, same-variable
	 * tracking, hop frames, element info arrays) were allocated in graph_cxt;
	 * delete it once to release them all.
	 */
	if (node->graph_cxt)
		MemoryContextDelete(node->graph_cxt);
}

/* ----------------------------------------------------------------
 *	 ExecReScanGraphScan
 * ----------------------------------------------------------------
 */
void
ExecReScanGraphScan(GraphScanState * node)
{
	int			i;

	if (node->start_vertex_slot)
	{
		ExecDropSingleTupleTableSlot(node->start_vertex_slot);
		node->start_vertex_slot = NULL;
	}

	free_datum_pair(&node->start_vertex_values, &node->start_vertex_nulls);
	node->n_start_keys = 0;

	if (node->hops)
	{
		for (i = 0; i < node->n_hops; i++)
			hop_reset(&node->hops[i]);
	}

	node->current_hop = -1;
	node->is_initialized = false;

	UpdateChangedParamSet(&node->ss.ps, node->ss.ps.chgParam);
}
