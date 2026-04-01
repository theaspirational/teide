/*
 * datalog.c — Datalog evaluation engine for Teide
 *
 * Compiles Datalog rules into td_graph_t operation DAGs and evaluates
 * them to fixpoint using semi-naive evaluation with stratified negation.
 */
#include "datalog.h"
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * Program lifecycle
 * ======================================================================== */

dl_program_t* dl_program_new(void) {
    /* Allocate via td_alloc and use the data region for the program struct.
     * This avoids alignment issues since td_alloc returns a td_t* header. */
    td_t* block = td_alloc(sizeof(dl_program_t));
    if (!block) return NULL;
    dl_program_t* prog = (dl_program_t*)td_data(block);
    memset(prog, 0, sizeof(dl_program_t));
    return prog;
}

/* Recover the td_t header from a dl_program_t pointer for td_free. */
static inline td_t* dl_prog_block(dl_program_t* prog) {
    return (td_t*)((char*)prog - 32);  /* td_data is at offset 32 */
}

void dl_program_free(dl_program_t* prog) {
    if (!prog) return;
    for (int i = 0; i < prog->n_rels; i++) {
        if (prog->rels[i].table && !TD_IS_ERR(prog->rels[i].table))
            td_release(prog->rels[i].table);
    }
    td_free(dl_prog_block(prog));
}

/* ========================================================================
 * Relation management
 * ======================================================================== */

int dl_find_rel(dl_program_t* prog, const char* name) {
    for (int i = 0; i < prog->n_rels; i++) {
        if (strcmp(prog->rels[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Generate a unique column name for a relation: "{relname}__c{idx}" */
static int64_t dl_col_sym(const char* rel_name, int col_idx) {
    char buf[80];
    snprintf(buf, sizeof(buf), "%s__c%d", rel_name, col_idx);
    return td_sym_intern(buf, strlen(buf));
}

int dl_add_edb(dl_program_t* prog, const char* name, td_t* table, int arity) {
    if (!prog || !name || !table || prog->n_rels >= DL_MAX_RELS)
        return -1;

    int idx = prog->n_rels++;
    dl_rel_t* rel = &prog->rels[idx];
    memset(rel, 0, sizeof(dl_rel_t));

    size_t name_len = strlen(name);
    if (name_len >= sizeof(rel->name)) name_len = sizeof(rel->name) - 1;
    memcpy(rel->name, name, name_len);
    rel->name[name_len] = '\0';

    rel->arity = arity;
    rel->is_idb = false;

    /* Build a new table with relation-prefixed column names to avoid
     * collisions when multiple tables participate in a join. */
    for (int c = 0; c < arity && c < DL_MAX_ARITY; c++)
        rel->col_names[c] = dl_col_sym(name, c);

    td_t* new_tbl = td_table_new(arity);
    for (int c = 0; c < arity; c++) {
        td_t* col = td_table_get_col_idx(table, c);
        if (col)
            new_tbl = td_table_add_col(new_tbl, rel->col_names[c], col);
    }
    rel->table = new_tbl;

    return idx;
}

int dl_ensure_idb(dl_program_t* prog, const char* name, int arity) {
    int idx = dl_find_rel(prog, name);
    if (idx >= 0) return idx;

    if (prog->n_rels >= DL_MAX_RELS) return -1;
    idx = prog->n_rels++;
    dl_rel_t* rel = &prog->rels[idx];
    memset(rel, 0, sizeof(dl_rel_t));

    size_t name_len = strlen(name);
    if (name_len >= sizeof(rel->name)) name_len = sizeof(rel->name) - 1;
    memcpy(rel->name, name, name_len);
    rel->name[name_len] = '\0';

    /* Create empty table with arity columns */
    rel->table = td_table_new(arity);
    if (!rel->table || TD_IS_ERR(rel->table)) return -1;

    rel->arity = arity;
    rel->is_idb = true;

    for (int c = 0; c < arity && c < DL_MAX_ARITY; c++) {
        rel->col_names[c] = dl_col_sym(name, c);
        td_t* empty_col = td_vec_new(TD_I64, 0);
        if (empty_col && !TD_IS_ERR(empty_col)) {
            rel->table = td_table_add_col(rel->table, rel->col_names[c], empty_col);
            td_release(empty_col);
        }
    }

    return idx;
}

/* ========================================================================
 * Rule management
 * ======================================================================== */

int dl_add_rule(dl_program_t* prog, const dl_rule_t* rule) {
    if (!prog || !rule || prog->n_rules >= DL_MAX_RULES)
        return -1;
    int idx = prog->n_rules++;
    memcpy(&prog->rules[idx], rule, sizeof(dl_rule_t));
    prog->rules[idx].stratum = -1;

    /* Ensure IDB relation exists for the head predicate */
    dl_ensure_idb(prog, rule->head_pred, rule->head_arity);

    return idx;
}

/* ========================================================================
 * Rule builder helpers
 * ======================================================================== */

void dl_rule_init(dl_rule_t* rule, const char* head_pred, int head_arity) {
    memset(rule, 0, sizeof(dl_rule_t));
    size_t len = strlen(head_pred);
    if (len >= sizeof(rule->head_pred)) len = sizeof(rule->head_pred) - 1;
    memcpy(rule->head_pred, head_pred, len);
    rule->head_pred[len] = '\0';
    rule->head_arity = head_arity;
    rule->n_body = 0;
    rule->n_vars = 0;
    rule->stratum = -1;
    for (int i = 0; i < DL_MAX_ARITY; i++)
        rule->head_vars[i] = DL_CONST;
}

void dl_rule_head_var(dl_rule_t* rule, int pos, int var_idx) {
    if (pos < 0 || pos >= rule->head_arity) return;
    rule->head_vars[pos] = var_idx;
    if (var_idx + 1 > rule->n_vars) rule->n_vars = var_idx + 1;
}

void dl_rule_head_const(dl_rule_t* rule, int pos, int64_t val) {
    if (pos < 0 || pos >= rule->head_arity) return;
    rule->head_vars[pos] = DL_CONST;
    rule->head_consts[pos] = val;
}

int dl_rule_add_atom(dl_rule_t* rule, const char* pred, int arity) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_POS;
    size_t len = strlen(pred);
    if (len >= sizeof(b->pred)) len = sizeof(b->pred) - 1;
    memcpy(b->pred, pred, len);
    b->pred[len] = '\0';
    b->arity = arity;
    for (int i = 0; i < DL_MAX_ARITY; i++)
        b->vars[i] = DL_CONST;
    return idx;
}

void dl_body_set_var(dl_rule_t* rule, int body_idx, int pos, int var_idx) {
    if (body_idx < 0 || body_idx >= rule->n_body) return;
    if (pos < 0 || pos >= rule->body[body_idx].arity) return;
    rule->body[body_idx].vars[pos] = var_idx;
    if (var_idx + 1 > rule->n_vars) rule->n_vars = var_idx + 1;
}

void dl_body_set_const(dl_rule_t* rule, int body_idx, int pos, int64_t val) {
    if (body_idx < 0 || body_idx >= rule->n_body) return;
    if (pos < 0 || pos >= rule->body[body_idx].arity) return;
    rule->body[body_idx].vars[pos] = DL_CONST;
    rule->body[body_idx].const_vals[pos] = val;
}

int dl_rule_add_neg(dl_rule_t* rule, const char* pred, int arity) {
    int idx = dl_rule_add_atom(rule, pred, arity);
    if (idx >= 0) rule->body[idx].type = DL_NEG;
    return idx;
}

int dl_rule_add_cmp(dl_rule_t* rule, int cmp_op, int lhs_var, int rhs_var) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_CMP;
    b->cmp_op = cmp_op;
    b->cmp_lhs = lhs_var;
    b->cmp_rhs = rhs_var;
    if (lhs_var + 1 > rule->n_vars) rule->n_vars = lhs_var + 1;
    if (rhs_var + 1 > rule->n_vars) rule->n_vars = rhs_var + 1;
    return idx;
}

int dl_rule_add_cmp_const(dl_rule_t* rule, int cmp_op, int lhs_var, int64_t rhs_val) {
    if (rule->n_body >= DL_MAX_BODY) return -1;
    int idx = rule->n_body++;
    dl_body_t* b = &rule->body[idx];
    memset(b, 0, sizeof(dl_body_t));
    b->type = DL_CMP;
    b->cmp_op = cmp_op;
    b->cmp_lhs = lhs_var;
    b->cmp_rhs = DL_CONST;
    b->cmp_const = rhs_val;
    if (lhs_var + 1 > rule->n_vars) rule->n_vars = lhs_var + 1;
    return idx;
}

/* ========================================================================
 * Stratification — topological sort on negation dependency graph
 * ======================================================================== */

int dl_stratify(dl_program_t* prog) {
    if (!prog) return -1;

    /* Build dependency graph: for each IDB predicate, which other IDB
     * predicates does it depend on positively or negatively? */
    int n = prog->n_rels;
    /* dep[i][j]: 0 = no dep, 1 = positive dep, 2 = negative dep */
    int dep[DL_MAX_RELS][DL_MAX_RELS];
    memset(dep, 0, sizeof(dep));

    for (int r = 0; r < prog->n_rules; r++) {
        dl_rule_t* rule = &prog->rules[r];
        int head_idx = dl_find_rel(prog, rule->head_pred);
        if (head_idx < 0) continue;

        for (int b = 0; b < rule->n_body; b++) {
            dl_body_t* body = &rule->body[b];
            if (body->type != DL_POS && body->type != DL_NEG) continue;
            int body_idx = dl_find_rel(prog, body->pred);
            if (body_idx < 0) continue;
            if (body->type == DL_NEG)
                dep[head_idx][body_idx] = 2;  /* negative dep */
            else if (dep[head_idx][body_idx] == 0)
                dep[head_idx][body_idx] = 1;  /* positive dep (don't override neg) */
        }
    }

    /* Assign strata: predicates with no negative dependencies go to stratum 0.
     * A predicate with a negative dep on stratum S goes to stratum S+1.
     * Repeat until stable. If unstable after n iterations, there's a cycle. */
    int stratum[DL_MAX_RELS];
    memset(stratum, 0, sizeof(stratum));

    for (int iter = 0; iter < n + 1; iter++) {
        bool changed = false;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                if (dep[i][j] == 2) {
                    /* Negative dependency: head must be in higher stratum */
                    if (stratum[i] <= stratum[j]) {
                        stratum[i] = stratum[j] + 1;
                        changed = true;
                    }
                } else if (dep[i][j] == 1) {
                    /* Positive dependency: head must be >= stratum */
                    if (stratum[i] < stratum[j]) {
                        stratum[i] = stratum[j];
                        changed = true;
                    }
                }
            }
        }
        if (!changed) break;
        if (iter == n) return -1;  /* unstratifiable negation cycle */
    }

    /* Build strata arrays */
    int max_stratum = 0;
    for (int i = 0; i < n; i++) {
        if (stratum[i] > max_stratum) max_stratum = stratum[i];
    }
    prog->n_strata = max_stratum + 1;
    memset(prog->strata_sizes, 0, sizeof(prog->strata_sizes));

    for (int i = 0; i < n; i++) {
        int s = stratum[i];
        if (s < DL_MAX_STRATA && prog->strata_sizes[s] < DL_MAX_RELS) {
            prog->strata[s][prog->strata_sizes[s]++] = i;
        }
    }

    /* Assign stratum to each rule */
    for (int r = 0; r < prog->n_rules; r++) {
        int head_idx = dl_find_rel(prog, prog->rules[r].head_pred);
        if (head_idx >= 0)
            prog->rules[r].stratum = stratum[head_idx];
    }

    return 0;
}

/* ========================================================================
 * Rule compiler — compile a single rule body into a td_graph_t DAG
 * ======================================================================== */

/* Helper: map a Datalog variable to the graph node that currently binds it.
 * var_bindings[var_idx] = the td_op_t* scan node for that variable's column.
 * var_table_id[var_idx] = which registered table the binding came from. */

td_op_t* dl_compile_rule(dl_program_t* prog, dl_rule_t* rule,
                          int delta_pos, td_graph_t* g) {
    /* Track variable bindings: var -> (table_id, column_name) */
    td_op_t* var_scan[DL_MAX_ARITY * DL_MAX_BODY]; /* scan node per variable */
    uint16_t var_tid[DL_MAX_ARITY * DL_MAX_BODY];   /* table id per variable */
    td_op_t* var_tbl[DL_MAX_ARITY * DL_MAX_BODY];   /* const_table node per variable */
    bool var_bound[DL_MAX_ARITY * DL_MAX_BODY];
    memset(var_bound, 0, sizeof(var_bound));
    memset(var_scan, 0, sizeof(var_scan));
    memset(var_tid, 0, sizeof(var_tid));
    memset(var_tbl, 0, sizeof(var_tbl));

    /* Process positive body atoms: build scans and joins */
    td_op_t* current = NULL;  /* current pipeline output node */
    uint16_t current_tid = 0;

    for (int b = 0; b < rule->n_body; b++) {
        dl_body_t* body = &rule->body[b];
        if (body->type != DL_POS) continue;

        /* Find the relation */
        int rel_idx = dl_find_rel(prog, body->pred);
        if (rel_idx < 0) return NULL;
        dl_rel_t* rel = &prog->rels[rel_idx];

        /* Use delta relation at delta_pos, full relation otherwise */
        td_t* scan_table;
        if (b == delta_pos) {
            /* For semi-naive: the delta relation is stored temporarily
             * in the next table slot. We'll pass it via the table registry. */
            scan_table = rel->table;  /* caller replaces with delta */
        } else {
            scan_table = rel->table;
        }

        /* Build a body-atom-specific copy of the table with unique column names
         * "b{body_idx}__c{col}" to avoid collisions when the same relation
         * appears multiple times in a rule body. */
        td_t* body_tbl = td_table_new(body->arity);
        for (int c = 0; c < body->arity; c++) {
            td_t* col = td_table_get_col_idx(scan_table, c);
            if (!col) continue;
            char col_name[80];
            snprintf(col_name, sizeof(col_name), "b%d__c%d", b, c);
            int64_t col_sym = td_sym_intern(col_name, strlen(col_name));
            body_tbl = td_table_add_col(body_tbl, col_sym, col);
        }

        /* Register the body-specific table in graph */
        uint16_t tid = td_graph_add_table(g, body_tbl);

        /* Create scan nodes with body-unique column names */
        td_op_t* col_scans[DL_MAX_ARITY];
        for (int c = 0; c < body->arity; c++) {
            char col_name[80];
            snprintf(col_name, sizeof(col_name), "b%d__c%d", b, c);
            col_scans[c] = td_scan_table(g, tid, col_name);
        }
        /* Replace scan_table with the body-specific table for const_table */
        scan_table = body_tbl;

        /* Apply constant filters */
        for (int c = 0; c < body->arity; c++) {
            if (body->vars[c] == DL_CONST) {
                td_op_t* const_node = td_const_i64(g, body->const_vals[c]);
                td_op_t* eq_node = td_eq(g, col_scans[c], const_node);
                td_op_t* filt = td_filter(g, col_scans[c], eq_node);
                (void)filt;
            }
        }

        /* Bind variables and track join keys */
        if (current == NULL) {
            /* First body atom — just record bindings */
            td_op_t* const_tbl = td_const_table(g, scan_table);
            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (!var_bound[v]) {
                    var_bound[v] = true;
                    var_scan[v] = col_scans[c];
                    var_tid[v] = tid;
                    var_tbl[v] = const_tbl;
                }
            }
            current = const_tbl;
            current_tid = tid;
        } else {
            /* Subsequent body atom — need to join with current */
            td_op_t* right_tbl = td_const_table(g, scan_table);

            /* Find shared variables = join keys */
            td_op_t* lkeys[DL_MAX_ARITY];
            td_op_t* rkeys[DL_MAX_ARITY];
            uint8_t n_join_keys = 0;

            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (var_bound[v]) {
                    /* Shared variable — this is a join key */
                    lkeys[n_join_keys] = var_scan[v];
                    rkeys[n_join_keys] = col_scans[c];
                    n_join_keys++;
                }
            }

            if (n_join_keys > 0) {
                current = td_join(g, current, lkeys, right_tbl, rkeys,
                                  n_join_keys, 0 /* inner join */);
            } else {
                /* Cross product (no shared vars) — still join with no keys.
                 * Use a dummy constant key that always matches. */
                td_op_t* l_one = td_const_i64(g, 1);
                td_op_t* r_one = td_const_i64(g, 1);
                td_op_t* lk[] = { l_one };
                td_op_t* rk[] = { r_one };
                current = td_join(g, current, lk, right_tbl, rk, 1, 0);
            }

            /* Bind any new variables from this atom */
            for (int c = 0; c < body->arity; c++) {
                int v = body->vars[c];
                if (v == DL_CONST) continue;
                if (!var_bound[v]) {
                    var_bound[v] = true;
                    var_scan[v] = col_scans[c];
                    var_tid[v] = tid;
                    var_tbl[v] = right_tbl;
                }
            }
        }
    }

    if (!current) return NULL;

    /* Process negated atoms: antijoin */
    for (int b = 0; b < rule->n_body; b++) {
        dl_body_t* body = &rule->body[b];
        if (body->type != DL_NEG) continue;

        int rel_idx = dl_find_rel(prog, body->pred);
        if (rel_idx < 0) return NULL;
        dl_rel_t* rel = &prog->rels[rel_idx];

        /* Build body-specific table with unique column names */
        td_t* neg_tbl = td_table_new(body->arity);
        for (int c = 0; c < body->arity; c++) {
            td_t* col = td_table_get_col_idx(rel->table, c);
            if (!col) continue;
            char col_name[80];
            snprintf(col_name, sizeof(col_name), "b%d__c%d", b, c);
            int64_t col_sym = td_sym_intern(col_name, strlen(col_name));
            neg_tbl = td_table_add_col(neg_tbl, col_sym, col);
        }

        uint16_t tid = td_graph_add_table(g, neg_tbl);
        td_op_t* right_tbl = td_const_table(g, neg_tbl);

        td_op_t* col_scans[DL_MAX_ARITY];
        for (int c = 0; c < body->arity; c++) {
            char col_name[80];
            snprintf(col_name, sizeof(col_name), "b%d__c%d", b, c);
            col_scans[c] = td_scan_table(g, tid, col_name);
        }

        /* Build antijoin keys: shared variables between current and negated atom */
        td_op_t* lkeys[DL_MAX_ARITY];
        td_op_t* rkeys[DL_MAX_ARITY];
        uint8_t n_keys = 0;

        for (int c = 0; c < body->arity; c++) {
            int v = body->vars[c];
            if (v == DL_CONST) continue;
            if (var_bound[v]) {
                lkeys[n_keys] = var_scan[v];
                rkeys[n_keys] = col_scans[c];
                n_keys++;
            }
        }

        if (n_keys > 0) {
            current = td_antijoin(g, current, lkeys, right_tbl, rkeys, n_keys);
        }
    }

    /* Process comparisons: filter */
    for (int b = 0; b < rule->n_body; b++) {
        dl_body_t* body = &rule->body[b];
        if (body->type != DL_CMP) continue;

        td_op_t* lhs = var_scan[body->cmp_lhs];
        td_op_t* rhs;
        if (body->cmp_rhs == DL_CONST)
            rhs = td_const_i64(g, body->cmp_const);
        else
            rhs = var_scan[body->cmp_rhs];

        if (!lhs || !rhs) continue;

        td_op_t* cmp_node = NULL;
        switch (body->cmp_op) {
            case DL_CMP_EQ: cmp_node = td_eq(g, lhs, rhs); break;
            case DL_CMP_NE: cmp_node = td_ne(g, lhs, rhs); break;
            case DL_CMP_LT: cmp_node = td_lt(g, lhs, rhs); break;
            case DL_CMP_LE: cmp_node = td_le(g, lhs, rhs); break;
            case DL_CMP_GT: cmp_node = td_gt(g, lhs, rhs); break;
            case DL_CMP_GE: cmp_node = td_ge(g, lhs, rhs); break;
            default: continue;
        }
        if (cmp_node)
            current = td_filter(g, current, cmp_node);
    }

    /* Project to head variables using td_select */
    td_op_t* head_cols[DL_MAX_ARITY];
    for (int c = 0; c < rule->head_arity; c++) {
        int v = rule->head_vars[c];
        if (v == DL_CONST) {
            head_cols[c] = td_const_i64(g, rule->head_consts[c]);
        } else {
            head_cols[c] = var_scan[v];
        }
    }

    td_op_t* projected = td_select(g, current, head_cols, (uint8_t)rule->head_arity);
    return projected;
}

/* ========================================================================
 * Table utilities for fixpoint evaluation
 * ======================================================================== */

/* Rename table columns to match the head relation's expected names.
 * This is needed because td_select output column names come from the scan
 * nodes (e.g., "edge__c0"), but we need them to match the head relation
 * (e.g., "path__c0"). Returns a new owned table. */
static td_t* table_rename_cols(td_t* tbl, dl_rel_t* target_rel) {
    if (!tbl || TD_IS_ERR(tbl)) return tbl;
    int64_t nrows = td_table_nrows(tbl);
    int64_t ncols = td_table_ncols(tbl);
    if (ncols <= 0) { td_retain(tbl); return tbl; }

    int arity = target_rel->arity;
    if (ncols != arity) { td_retain(tbl); return tbl; }

    /* Check if renaming is needed */
    bool needs_rename = false;
    for (int c = 0; c < arity; c++) {
        if (td_table_col_name(tbl, c) != target_rel->col_names[c]) {
            needs_rename = true;
            break;
        }
    }
    if (!needs_rename) { td_retain(tbl); return tbl; }

    /* Build new table with correct column names sharing the same column data */
    td_t* out = td_table_new(arity);
    for (int c = 0; c < arity; c++) {
        td_t* col = td_table_get_col_idx(tbl, c);
        if (col)
            out = td_table_add_col(out, target_rel->col_names[c], col);
    }
    return out;
}

/* Canonicalize column names to "c0","c1",... Returns new owned table. */
static td_t* canonicalize(td_t* tbl) {
    if (!tbl || TD_IS_ERR(tbl)) return tbl;
    int64_t nc = td_table_ncols(tbl);
    td_t* out = td_table_new(nc);
    for (int64_t c = 0; c < nc; c++) {
        td_t* col = td_table_get_col_idx(tbl, c);
        if (!col) continue;
        char buf[16];
        snprintf(buf, sizeof(buf), "c%d", (int)c);
        int64_t sym = td_sym_intern(buf, strlen(buf));
        out = td_table_add_col(out, sym, col);
    }
    return out;
}

/* Restore original column names from `src` onto `tbl`. */
static td_t* restore_names(td_t* tbl, td_t* src) {
    if (!tbl || TD_IS_ERR(tbl)) return tbl;
    int64_t nc = td_table_ncols(tbl);
    td_t* out = td_table_new(nc);
    for (int64_t c = 0; c < nc; c++) {
        td_t* col = td_table_get_col_idx(tbl, c);
        if (col)
            out = td_table_add_col(out, td_table_col_name(src, c), col);
    }
    td_release(tbl);
    return out;
}

/* Create a table by concatenating all rows from tables a and b (same schema).
 * Uses td_union_all via operation graph. Returns new owned table with a's names. */
static td_t* table_union(td_t* a, td_t* b) {
    if (!a || TD_IS_ERR(a)) return b;
    if (!b || TD_IS_ERR(b)) return a;
    if (td_table_nrows(a) == 0) { td_retain(b); return b; }
    if (td_table_nrows(b) == 0) { td_retain(a); return a; }

    /* Canonicalize both to avoid column name mismatch in union_all */
    td_t* ca = canonicalize(a);
    td_t* cb = canonicalize(b);

    td_graph_t* g = td_graph_new(NULL);
    if (!g) { td_release(ca); td_release(cb); td_retain(a); return a; }

    td_op_t* left = td_const_table(g, ca);
    td_op_t* right = td_const_table(g, cb);
    td_op_t* u = td_union_all(g, left, right);

    td_t* raw = td_execute(g, u);
    td_graph_free(g);
    td_release(ca);
    td_release(cb);

    /* Restore original column names from a */
    return restore_names(raw, a);
}

/* Deduplicate table rows on all columns. Returns new owned table. */
static td_t* table_distinct(td_t* tbl) {
    if (!tbl || TD_IS_ERR(tbl)) return tbl;
    int64_t nrows = td_table_nrows(tbl);
    if (nrows <= 1) { td_retain(tbl); return tbl; }

    int64_t ncols = td_table_ncols(tbl);
    if (ncols <= 0) { td_retain(tbl); return tbl; }

    td_t* canonical = canonicalize(tbl);

    td_graph_t* g = td_graph_new(canonical);
    if (!g) { td_release(canonical); td_retain(tbl); return tbl; }

    td_op_t* keys[DL_MAX_ARITY];
    for (int64_t c = 0; c < ncols && c < DL_MAX_ARITY; c++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "c%d", (int)c);
        keys[c] = td_scan(g, buf);
    }

    td_op_t* dist = td_distinct(g, keys, (uint8_t)ncols);
    td_optimize(g, dist);
    td_t* deduped = td_execute(g, dist);
    td_graph_free(g);
    td_release(canonical);

    return restore_names(deduped, tbl);
}

/* Anti-join: rows in `left` that don't appear in `right` (same schema).
 * Returns new owned table with left's original column names. */
static td_t* table_antijoin(td_t* left, td_t* right) {
    if (!left || TD_IS_ERR(left)) return left;
    if (!right || TD_IS_ERR(right) || td_table_nrows(right) == 0) {
        td_retain(left);
        return left;
    }
    if (td_table_nrows(left) == 0) {
        td_retain(left);
        return left;
    }

    int64_t ncols = td_table_ncols(left);
    if (ncols <= 0) { td_retain(left); return left; }

    td_t* cl = canonicalize(left);
    td_t* cr = canonicalize(right);

    td_graph_t* g = td_graph_new(NULL);
    if (!g) { td_release(cl); td_release(cr); td_retain(left); return left; }

    td_op_t* l = td_const_table(g, cl);
    td_op_t* r = td_const_table(g, cr);

    uint16_t l_tid = td_graph_add_table(g, cl);
    uint16_t r_tid = td_graph_add_table(g, cr);

    td_op_t* lkeys[DL_MAX_ARITY];
    td_op_t* rkeys[DL_MAX_ARITY];
    for (int64_t c = 0; c < ncols && c < DL_MAX_ARITY; c++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "c%d", (int)c);
        lkeys[c] = td_scan_table(g, l_tid, buf);
        rkeys[c] = td_scan_table(g, r_tid, buf);
    }

    td_op_t* aj = td_antijoin(g, l, lkeys, r, rkeys, (uint8_t)ncols);
    td_t* raw = td_execute(g, aj);
    td_graph_free(g);
    td_release(cl);
    td_release(cr);

    return restore_names(raw, left);
}

/* Normalize column names of a table to match the target relation's "c0","c1"... scheme.
 * Returns a new owned table with correct names (shares column data). */
static td_t* normalize_columns(td_t* tbl, dl_rel_t* rel) {
    if (!tbl || TD_IS_ERR(tbl)) return tbl;
    int64_t ncols = td_table_ncols(tbl);
    if (ncols != rel->arity) {
        /* Arity mismatch — can't normalize */
        td_retain(tbl);
        return tbl;
    }
    /* Check if already correct */
    bool ok = true;
    for (int c = 0; c < rel->arity; c++) {
        if (td_table_col_name(tbl, c) != rel->col_names[c]) { ok = false; break; }
    }
    if (ok) { td_retain(tbl); return tbl; }

    /* Rebuild with correct names, sharing column data */
    td_t* out = td_table_new(rel->arity);
    for (int c = 0; c < rel->arity; c++) {
        td_t* col = td_table_get_col_idx(tbl, c);
        if (col)
            out = td_table_add_col(out, rel->col_names[c], col);
    }
    return out;
}

/* ========================================================================
 * Semi-naive fixpoint evaluation
 * ======================================================================== */

int dl_eval(dl_program_t* prog) {
    if (!prog) return -1;

    /* Stratify if not already done */
    if (prog->n_strata == 0) {
        if (dl_stratify(prog) != 0) return -1;
    }

    /* Process each stratum */
    for (int s = 0; s < prog->n_strata; s++) {
        /* Collect rules in this stratum */
        dl_rule_t* stratum_rules[DL_MAX_RULES];
        int n_stratum_rules = 0;

        for (int r = 0; r < prog->n_rules; r++) {
            if (prog->rules[r].stratum == s) {
                stratum_rules[n_stratum_rules++] = &prog->rules[r];
            }
        }
        if (n_stratum_rules == 0) continue;

        /* Phase A: Initial evaluation — evaluate each rule with full relations */
        /* Group rules by head predicate */
        for (int ri = 0; ri < n_stratum_rules; ri++) {
            dl_rule_t* rule = stratum_rules[ri];
            int head_idx = dl_find_rel(prog, rule->head_pred);
            if (head_idx < 0) continue;
            dl_rel_t* head_rel = &prog->rels[head_idx];

            td_graph_t* g = td_graph_new(NULL);
            if (!g) continue;

            td_op_t* output = dl_compile_rule(prog, rule, -1, g);
            if (!output) { td_graph_free(g); continue; }

            td_t* raw_tuples = td_execute(g, output);
            td_graph_free(g);

            if (!raw_tuples || TD_IS_ERR(raw_tuples)) continue;

            /* Rename columns to match head relation's expected names */
            td_t* new_tuples = table_rename_cols(raw_tuples, head_rel);
            td_release(raw_tuples);
            if (!new_tuples || TD_IS_ERR(new_tuples)) continue;

            /* Merge into the head relation's table */
            td_t* merged = table_union(head_rel->table, new_tuples);
            td_release(new_tuples);
            if (merged && !TD_IS_ERR(merged)) {
                td_t* deduped = table_distinct(merged);
                td_release(merged);
                if (deduped && !TD_IS_ERR(deduped)) {
                    td_release(head_rel->table);
                    head_rel->table = deduped;
                }
            }
        }

        /* Phase B: Semi-naive loop — iterate with delta relations */
        /* For each IDB predicate in this stratum, compute delta as the
         * difference between current and previous table states. */
        td_t* prev_tables[DL_MAX_RELS];
        td_t* delta_tables[DL_MAX_RELS];
        memset(prev_tables, 0, sizeof(prev_tables));
        memset(delta_tables, 0, sizeof(delta_tables));

        /* Initially, delta = full table (all tuples are new) */
        for (int p = 0; p < prog->strata_sizes[s]; p++) {
            int rel_idx = prog->strata[s][p];
            dl_rel_t* rel = &prog->rels[rel_idx];
            if (rel->is_idb) {
                td_retain(rel->table);
                delta_tables[rel_idx] = rel->table;
                /* prev = empty table with same schema as the relation */
                prev_tables[rel_idx] = td_table_new(rel->arity);
                for (int c = 0; c < rel->arity && c < DL_MAX_ARITY; c++) {
                    td_t* empty_col = td_vec_new(TD_I64, 0);
                    if (empty_col && !TD_IS_ERR(empty_col)) {
                        prev_tables[rel_idx] = td_table_add_col(
                            prev_tables[rel_idx], rel->col_names[c], empty_col);
                        td_release(empty_col);
                    }
                }
            }
        }

        /* Semi-naive iteration */
        int max_iter = 1000;
        for (int iter = 0; iter < max_iter; iter++) {
            /* Check convergence: all deltas empty */
            bool any_new = false;
            for (int p = 0; p < prog->strata_sizes[s]; p++) {
                int rel_idx = prog->strata[s][p];
                if (delta_tables[rel_idx] &&
                    !TD_IS_ERR(delta_tables[rel_idx]) &&
                    td_table_nrows(delta_tables[rel_idx]) > 0) {
                    any_new = true;
                    break;
                }
            }
            if (!any_new) break;

            /* For each rule, for each positive body position that uses a
             * delta relation, compile and execute */
            td_t* new_tuples_per_rel[DL_MAX_RELS];
            memset(new_tuples_per_rel, 0, sizeof(new_tuples_per_rel));

            for (int ri = 0; ri < n_stratum_rules; ri++) {
                dl_rule_t* rule = stratum_rules[ri];
                int head_idx = dl_find_rel(prog, rule->head_pred);
                if (head_idx < 0) continue;

                for (int b = 0; b < rule->n_body; b++) {
                    dl_body_t* body = &rule->body[b];
                    if (body->type != DL_POS) continue;

                    int body_rel = dl_find_rel(prog, body->pred);
                    if (body_rel < 0) continue;
                    if (!prog->rels[body_rel].is_idb) continue;
                    if (!delta_tables[body_rel] ||
                        td_table_nrows(delta_tables[body_rel]) == 0) continue;

                    /* Swap in delta relation for this body position */
                    td_t* saved = prog->rels[body_rel].table;
                    prog->rels[body_rel].table = delta_tables[body_rel];

                    td_graph_t* g = td_graph_new(NULL);
                    if (!g) { prog->rels[body_rel].table = saved; continue; }

                    td_op_t* output = dl_compile_rule(prog, rule, b, g);
                    if (!output) {
                        td_graph_free(g);
                        prog->rels[body_rel].table = saved;
                        continue;
                    }

                    td_t* raw_result = td_execute(g, output);
                    td_graph_free(g);
                    prog->rels[body_rel].table = saved;

                    if (!raw_result || TD_IS_ERR(raw_result)) continue;

                    /* Rename columns to match head relation */
                    dl_rel_t* head_rel2 = &prog->rels[head_idx];
                    td_t* result = table_rename_cols(raw_result, head_rel2);
                    td_release(raw_result);
                    if (!result || TD_IS_ERR(result)) continue;

                    /* Accumulate new tuples for this head */
                    if (new_tuples_per_rel[head_idx]) {
                        td_t* u = table_union(new_tuples_per_rel[head_idx], result);
                        td_release(new_tuples_per_rel[head_idx]);
                        td_release(result);
                        new_tuples_per_rel[head_idx] = u;
                    } else {
                        new_tuples_per_rel[head_idx] = result;
                    }
                }
            }

            /* For each IDB: dedup new tuples, subtract existing, merge */
            for (int p = 0; p < prog->strata_sizes[s]; p++) {
                int rel_idx = prog->strata[s][p];
                dl_rel_t* rel = &prog->rels[rel_idx];
                if (!rel->is_idb) continue;

                /* Free old delta */
                if (delta_tables[rel_idx] && !TD_IS_ERR(delta_tables[rel_idx]))
                    td_release(delta_tables[rel_idx]);
                delta_tables[rel_idx] = NULL;

                td_t* new_tuples = new_tuples_per_rel[rel_idx];
                if (!new_tuples || TD_IS_ERR(new_tuples)) {
                    delta_tables[rel_idx] = NULL;
                    continue;
                }

                /* Deduplicate */
                td_t* deduped = table_distinct(new_tuples);
                td_release(new_tuples);
                if (!deduped || TD_IS_ERR(deduped)) continue;

                /* Subtract existing relation to get true delta */
                td_t* delta = table_antijoin(deduped, rel->table);
                td_release(deduped);
                if (!delta || TD_IS_ERR(delta)) continue;

                delta_tables[rel_idx] = delta;

                /* Merge delta into full relation */
                if (td_table_nrows(delta) > 0) {
                    td_t* merged = table_union(rel->table, delta);
                    if (merged && !TD_IS_ERR(merged)) {
                        td_release(rel->table);
                        rel->table = merged;
                    }
                }
            }

            /* Update prev tables */
            for (int p = 0; p < prog->strata_sizes[s]; p++) {
                int rel_idx = prog->strata[s][p];
                if (prev_tables[rel_idx] && !TD_IS_ERR(prev_tables[rel_idx]))
                    td_release(prev_tables[rel_idx]);
                td_retain(prog->rels[rel_idx].table);
                prev_tables[rel_idx] = prog->rels[rel_idx].table;
            }
        }

        /* Cleanup stratum temporaries */
        for (int p = 0; p < prog->strata_sizes[s]; p++) {
            int rel_idx = prog->strata[s][p];
            if (prev_tables[rel_idx] && !TD_IS_ERR(prev_tables[rel_idx]))
                td_release(prev_tables[rel_idx]);
            if (delta_tables[rel_idx] && !TD_IS_ERR(delta_tables[rel_idx]))
                td_release(delta_tables[rel_idx]);
        }
    }

    return 0;
}

/* ========================================================================
 * Query — retrieve result after evaluation
 * ======================================================================== */

td_t* dl_query(dl_program_t* prog, const char* pred_name) {
    if (!prog || !pred_name) return NULL;
    int idx = dl_find_rel(prog, pred_name);
    if (idx < 0) return NULL;
    return prog->rels[idx].table;
}
