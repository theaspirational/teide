/*
 * datalog.h — Datalog evaluation engine for Teide
 *
 * Compiles Datalog rules into td_graph_t operation DAGs and evaluates
 * them to fixpoint using Teide's vectorized columnar execution engine.
 * Supports semi-naive evaluation, stratified negation, and multi-rule heads.
 */
#ifndef TEIDE_DATALOG_H
#define TEIDE_DATALOG_H

#include "teide/td.h"
#include <stdbool.h>
#include <stdint.h>

/* ===== Body literal types ===== */
#define DL_POS      0   /* positive atom: pred(X, Y, ...) */
#define DL_NEG      1   /* negated atom:  not pred(X, Y, ...) */
#define DL_CMP      2   /* comparison:    X < Y, X = c, etc. */
#define DL_ASSIGN   3   /* assignment:    X = expr (reserved for future) */

/* ===== Comparison operators (for DL_CMP) ===== */
#define DL_CMP_EQ   0
#define DL_CMP_NE   1
#define DL_CMP_LT   2
#define DL_CMP_LE   3
#define DL_CMP_GT   4
#define DL_CMP_GE   5

/* Variable index sentinel: constant value, not a variable */
#define DL_CONST    (-1)

/* Maximum arity for any relation */
#define DL_MAX_ARITY 16

/* Maximum number of body literals per rule */
#define DL_MAX_BODY  16

/* Maximum number of rules in a program */
#define DL_MAX_RULES 128

/* Maximum number of relations */
#define DL_MAX_RELS  64

/* Maximum strata */
#define DL_MAX_STRATA 16

/* ===== Body literal ===== */
typedef struct {
    int     type;                   /* DL_POS, DL_NEG, DL_CMP, DL_ASSIGN */
    char    pred[64];               /* predicate name (for DL_POS/DL_NEG) */
    int     arity;                  /* number of argument positions */
    int     vars[DL_MAX_ARITY];    /* variable indices (DL_CONST for constants) */
    int64_t const_vals[DL_MAX_ARITY]; /* constant values (I64/SYM) */
    int     cmp_op;                /* comparison operator (for DL_CMP) */
    int     cmp_lhs;               /* left variable index (for DL_CMP) */
    int     cmp_rhs;               /* right variable index or DL_CONST */
    int64_t cmp_const;             /* constant value if cmp_rhs == DL_CONST */
} dl_body_t;

/* ===== Datalog rule: head :- body ===== */
typedef struct {
    char    head_pred[64];          /* head predicate name */
    int     head_arity;
    int     head_vars[DL_MAX_ARITY]; /* variable indices in head */
    int64_t head_consts[DL_MAX_ARITY]; /* constants (when head_vars[i] == DL_CONST) */
    int     n_body;                 /* number of body literals */
    dl_body_t body[DL_MAX_BODY];
    int     n_vars;                 /* total distinct variable count in rule */
    int     stratum;                /* assigned stratum (-1 if not yet stratified) */
} dl_rule_t;

/* ===== Datalog relation ===== */
typedef struct {
    char    name[64];               /* relation name */
    td_t*   table;                  /* backing columnar table */
    int     arity;                  /* number of columns */
    bool    is_idb;                 /* true = derived (intensional) */
    int64_t col_names[DL_MAX_ARITY]; /* interned column name symbols */
} dl_rel_t;

/* ===== Datalog program ===== */
typedef struct {
    dl_rel_t    rels[DL_MAX_RELS];
    int         n_rels;
    dl_rule_t   rules[DL_MAX_RULES];
    int         n_rules;
    int         strata[DL_MAX_STRATA][DL_MAX_RELS]; /* predicate indices per stratum */
    int         strata_sizes[DL_MAX_STRATA];         /* number of predicates per stratum */
    int         n_strata;
} dl_program_t;

/* ===== Public API ===== */

/* Create a new empty Datalog program */
dl_program_t* dl_program_new(void);

/* Free a Datalog program and release all owned tables */
void dl_program_free(dl_program_t* prog);

/* Register an EDB (extensional) relation backed by an existing table.
 * Column names are auto-generated as "c0", "c1", ... unless the table
 * already has named columns. */
int dl_add_edb(dl_program_t* prog, const char* name, td_t* table, int arity);

/* Add a rule to the program. The rule struct is copied. */
int dl_add_rule(dl_program_t* prog, const dl_rule_t* rule);

/* Compute stratification (topological sort of negation dependency graph).
 * Returns 0 on success, -1 if program has unstratifiable negation cycle. */
int dl_stratify(dl_program_t* prog);

/* Evaluate the program to fixpoint using semi-naive evaluation.
 * Returns 0 on success, -1 on error. */
int dl_eval(dl_program_t* prog);

/* Query the result of a derived relation after evaluation.
 * Returns the backing table (caller does NOT own it). */
td_t* dl_query(dl_program_t* prog, const char* pred_name);

/* ===== Rule builder helpers ===== */

/* Initialize a rule with the given head predicate and arity */
void dl_rule_init(dl_rule_t* rule, const char* head_pred, int head_arity);

/* Set a head argument to a variable */
void dl_rule_head_var(dl_rule_t* rule, int pos, int var_idx);

/* Set a head argument to a constant */
void dl_rule_head_const(dl_rule_t* rule, int pos, int64_t val);

/* Add a positive body atom. Returns body literal index. */
int dl_rule_add_atom(dl_rule_t* rule, const char* pred, int arity);

/* Set a body atom argument to a variable */
void dl_body_set_var(dl_rule_t* rule, int body_idx, int pos, int var_idx);

/* Set a body atom argument to a constant */
void dl_body_set_const(dl_rule_t* rule, int body_idx, int pos, int64_t val);

/* Add a negated body atom. Returns body literal index. */
int dl_rule_add_neg(dl_rule_t* rule, const char* pred, int arity);

/* Add a comparison. Returns body literal index. */
int dl_rule_add_cmp(dl_rule_t* rule, int cmp_op, int lhs_var, int rhs_var);

/* Add a comparison with a constant RHS. Returns body literal index. */
int dl_rule_add_cmp_const(dl_rule_t* rule, int cmp_op, int lhs_var, int64_t rhs_val);

/* ===== Internal (used by compiler) ===== */

/* Find relation by name. Returns index or -1. */
int dl_find_rel(dl_program_t* prog, const char* name);

/* Ensure an IDB relation exists for the given head predicate.
 * Creates it with the correct arity if it doesn't exist yet. */
int dl_ensure_idb(dl_program_t* prog, const char* name, int arity);

/* Compile one rule into a td_graph_t for one fixpoint iteration.
 * delta_pos: which body atom uses the delta relation (-1 for initial pass).
 * Returns the output node in g that produces new head tuples. */
td_op_t* dl_compile_rule(dl_program_t* prog, dl_rule_t* rule,
                          int delta_pos, td_graph_t* g);

#endif /* TEIDE_DATALOG_H */
