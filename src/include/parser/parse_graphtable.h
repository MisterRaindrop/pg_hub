/*-------------------------------------------------------------------------
 *
 * parse_graphtable.h
 *		parsing of GRAPH_TABLE
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_graphtable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_GRAPHTABLE_H
#define PARSE_GRAPHTABLE_H

#include "nodes/pg_list.h"
#include "parser/parse_node.h"

extern Node *transformGraphTablePropertyRef(ParseState *pstate, ColumnRef *cref);

extern Node *transformGraphPattern(ParseState *pstate, GraphPattern *graph_pattern);

/*
 * Collect the OIDs of the labels referenced by a label expression (a single
 * GraphLabelRef or a BoolExpr OR of GraphLabelRef nodes).  Returns NIL if
 * labelexpr is NULL or references no labels.  Shared by the parser, the
 * native executor's init-time validation, and the graph rewrite fallback.
 */
extern List *get_label_oids_for_labelexpr(Node *labelexpr);

/*
 * Return the OIDs of all labels belonging to the given property graph.
 * Used for a graph element pattern without a label expression, which
 * matches every label of the graph.
 */
extern List *get_graph_all_label_oids(Oid propgraphid);

/*
 * Map a graph element pattern kind to the element-kind character ('v'/'e')
 * and class name ("vertex"/"edge"); returns false for non vertex/edge kinds.
 * Shared by the parser validators, the native planner, and the executor.
 */
extern bool graph_element_kind_info(GraphElementPatternKind kind,
									char *element_kind, const char **kind_str);

/*
 * Verify that every label explicitly referenced by the label expressions in
 * the given graph pattern is associated with some element of the kind
 * (vertex or edge) the corresponding element pattern requires; error
 * otherwise.  Called at parse time and again at executor init (to reflect
 * DDL after parsing, e.g. for cached plans).
 */
extern void validate_graph_element_label_kinds(GraphPattern *pattern,
											   Oid graph_oid);

/*
 * Callback used by graph_label_expr_matches(): does the element described by
 * arg carry the given label?
 */
typedef bool (*GraphLabelHasFn) (Oid labelid, void *arg);

/*
 * Match a label expression (a single GraphLabelRef, or a BoolExpr OR tree of
 * GraphLabelRef nodes) against an element, invoking has_label for each label
 * the expression references.  A NULL labelexpr matches everything.  Shared by
 * the native planner (syscache-backed) and the native executor (cached
 * element model) so the OR / GraphLabelRef traversal is single-sourced.
 */
extern bool graph_label_expr_matches(Node *labelexpr,
									 GraphLabelHasFn has_label, void *arg);

#endif							/* PARSE_GRAPHTABLE_H */
