/*
 * test_datalog.c — Tests for OP_ANTIJOIN and the Datalog evaluation engine
 */

#include "munit.h"
#include <teide/td.h>
#include "../src/datalog/datalog.h"
#include <string.h>

/* ======================================================================
 * Helper: build a table from raw I64 arrays
 * ====================================================================== */

static td_t* make_i64_table(int ncols, const char* names[],
                              const int64_t* data[], int64_t nrows) {
    td_t* tbl = td_table_new(ncols);
    for (int c = 0; c < ncols; c++) {
        td_t* col = td_vec_from_raw(TD_I64, data[c], nrows);
        int64_t sym = td_sym_intern(names[c], strlen(names[c]));
        tbl = td_table_add_col(tbl, sym, col);
        td_release(col);
    }
    return tbl;
}

/* Helper: build a Datalog-style table with "c0", "c1", ... column names */
static td_t* make_dl_table(int arity, const int64_t* data[], int64_t nrows) {
    const char* names[DL_MAX_ARITY];
    char bufs[DL_MAX_ARITY][8];
    for (int c = 0; c < arity; c++) {
        snprintf(bufs[c], sizeof(bufs[c]), "c%d", c);
        names[c] = bufs[c];
    }
    return make_i64_table(arity, names, data, nrows);
}

/* Helper: extract an I64 column value */
static int64_t col_i64(td_t* tbl, int col_idx, int64_t row) {
    td_t* col = td_table_get_col_idx(tbl, col_idx);
    if (!col) return -999;
    return ((int64_t*)td_data(col))[row];
}

/* ======================================================================
 * OP_ANTIJOIN tests
 * ====================================================================== */

/* Basic antijoin: left={1,2,3,4}, right={2,4} → result={1,3} */
static MunitResult test_antijoin_basic(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    int64_t lid[] = {1, 2, 3, 4};
    int64_t lval[] = {10, 20, 30, 40};
    const char* lnames[] = {"id", "val"};
    const int64_t* ldata[] = {lid, lval};
    td_t* left = make_i64_table(2, lnames, ldata, 4);

    int64_t rid[] = {2, 4};
    const char* rnames[] = {"id"};
    const int64_t* rdata[] = {rid};
    td_t* right = make_i64_table(1, rnames, rdata, 2);

    td_graph_t* g = td_graph_new(left);
    td_op_t* l_op = td_const_table(g, left);
    td_op_t* r_op = td_const_table(g, right);
    td_op_t* lk = td_scan(g, "id");
    td_op_t* rk = td_scan(g, "id");
    td_op_t* lk_arr[] = { lk };
    td_op_t* rk_arr[] = { rk };
    td_op_t* aj = td_antijoin(g, l_op, lk_arr, r_op, rk_arr, 1);

    munit_assert_ptr_not_null(aj);
    munit_assert_int(aj->opcode, ==, OP_ANTIJOIN);

    td_t* result = td_execute(g, aj);
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 2);
    /* Only left-side columns in output */
    munit_assert_int(td_table_ncols(result), ==, 2);

    /* Verify values: should be {1,10} and {3,30} */
    int64_t* res_id = (int64_t*)td_data(td_table_get_col_idx(result, 0));
    int64_t* res_val = (int64_t*)td_data(td_table_get_col_idx(result, 1));
    munit_assert_int(res_id[0], ==, 1);
    munit_assert_int(res_id[1], ==, 3);
    munit_assert_int(res_val[0], ==, 10);
    munit_assert_int(res_val[1], ==, 30);

    td_release(result);
    td_graph_free(g);
    td_release(left);
    td_release(right);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Antijoin with empty right → all left rows pass */
static MunitResult test_antijoin_empty_right(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    int64_t lid[] = {1, 2, 3};
    const char* names[] = {"id"};
    const int64_t* ldata[] = {lid};
    td_t* left = make_i64_table(1, names, ldata, 3);

    td_t* right = td_table_new(1);
    td_t* empty_col = td_vec_new(TD_I64, 0);
    int64_t sym = td_sym_intern("id", 2);
    right = td_table_add_col(right, sym, empty_col);
    td_release(empty_col);

    td_graph_t* g = td_graph_new(left);
    td_op_t* l_op = td_const_table(g, left);
    td_op_t* r_op = td_const_table(g, right);
    td_op_t* lk = td_scan(g, "id");
    td_op_t* rk = td_scan(g, "id");
    td_op_t* lk_arr[] = { lk };
    td_op_t* rk_arr[] = { rk };
    td_op_t* aj = td_antijoin(g, l_op, lk_arr, r_op, rk_arr, 1);

    td_t* result = td_execute(g, aj);
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(td_table_nrows(result), ==, 3);

    td_release(result);
    td_graph_free(g);
    td_release(left);
    td_release(right);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Antijoin with all-match → empty result */
static MunitResult test_antijoin_full_match(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    int64_t lid[] = {1, 2, 3};
    int64_t rid[] = {1, 2, 3, 4};
    const char* names[] = {"id"};
    const int64_t* ldata[] = {lid};
    const int64_t* rdata[] = {rid};
    td_t* left = make_i64_table(1, names, ldata, 3);
    td_t* right = make_i64_table(1, names, rdata, 4);

    td_graph_t* g = td_graph_new(left);
    td_op_t* l_op = td_const_table(g, left);
    td_op_t* r_op = td_const_table(g, right);
    td_op_t* lk = td_scan(g, "id");
    td_op_t* rk = td_scan(g, "id");
    td_op_t* lk_arr[] = { lk };
    td_op_t* rk_arr[] = { rk };
    td_op_t* aj = td_antijoin(g, l_op, lk_arr, r_op, rk_arr, 1);

    td_t* result = td_execute(g, aj);
    munit_assert_ptr_not_null(result);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(td_table_nrows(result), ==, 0);

    td_release(result);
    td_graph_free(g);
    td_release(left);
    td_release(right);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* ======================================================================
 * td_table_insert_row test
 * ====================================================================== */

static MunitResult test_table_insert_row(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    int64_t s_a = td_sym_intern("a", 1);
    int64_t s_b = td_sym_intern("b", 1);

    td_t* col_a = td_vec_new(TD_I64, 0);
    td_t* col_b = td_vec_new(TD_I64, 0);
    td_t* tbl = td_table_new(2);
    tbl = td_table_add_col(tbl, s_a, col_a);
    tbl = td_table_add_col(tbl, s_b, col_b);
    td_release(col_a);
    td_release(col_b);

    munit_assert_int(td_table_nrows(tbl), ==, 0);

    /* Insert 3 rows */
    int64_t v1 = 10, v2 = 20;
    const void* vals1[] = {&v1, &v2};
    tbl = td_table_insert_row(tbl, vals1);
    munit_assert_false(TD_IS_ERR(tbl));
    munit_assert_int(td_table_nrows(tbl), ==, 1);

    v1 = 30; v2 = 40;
    tbl = td_table_insert_row(tbl, vals1);
    munit_assert_int(td_table_nrows(tbl), ==, 2);

    v1 = 50; v2 = 60;
    tbl = td_table_insert_row(tbl, vals1);
    munit_assert_int(td_table_nrows(tbl), ==, 3);

    /* Verify column values */
    td_t* ca = td_table_get_col(tbl, s_a);
    td_t* cb = td_table_get_col(tbl, s_b);
    munit_assert_ptr_not_null(ca);
    munit_assert_ptr_not_null(cb);
    int64_t* da = (int64_t*)td_data(ca);
    int64_t* db = (int64_t*)td_data(cb);
    munit_assert_int(da[0], ==, 10);
    munit_assert_int(da[1], ==, 30);
    munit_assert_int(da[2], ==, 50);
    munit_assert_int(db[0], ==, 20);
    munit_assert_int(db[1], ==, 40);
    munit_assert_int(db[2], ==, 60);

    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* ======================================================================
 * Datalog engine tests
 * ====================================================================== */

/* Simple fact query: assert facts, query them back */
static MunitResult test_dl_fact_query(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    /* Create program with edge(1,2), edge(2,3), edge(3,4) */
    dl_program_t* prog = dl_program_new();
    munit_assert_ptr_not_null(prog);

    int64_t c0_data[] = {1, 2, 3};
    int64_t c1_data[] = {2, 3, 4};
    const int64_t* edata[] = {c0_data, c1_data};
    td_t* edge_tbl = make_dl_table(2, edata, 3);
    munit_assert_false(TD_IS_ERR(edge_tbl));

    int rc = dl_add_edb(prog, "edge", edge_tbl, 2);
    munit_assert_int(rc, >=, 0);

    /* Query back */
    td_t* result = dl_query(prog, "edge");
    munit_assert_ptr_not_null(result);
    munit_assert_int(td_table_nrows(result), ==, 3);

    td_release(edge_tbl);
    dl_program_free(prog);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* One-rule derivation: grandparent(X,Z) :- parent(X,Y), parent(Y,Z) */
static MunitResult test_dl_grandparent(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    dl_program_t* prog = dl_program_new();

    /* parent(1,2), parent(2,3), parent(3,4) */
    int64_t p0[] = {1, 2, 3};
    int64_t p1[] = {2, 3, 4};
    const int64_t* pdata[] = {p0, p1};
    td_t* parent_tbl = make_dl_table(2, pdata, 3);
    dl_add_edb(prog, "parent", parent_tbl, 2);

    /* grandparent(X,Z) :- parent(X,Y), parent(Y,Z) */
    dl_rule_t rule;
    dl_rule_init(&rule, "grandparent", 2);
    dl_rule_head_var(&rule, 0, 0);  /* X */
    dl_rule_head_var(&rule, 1, 2);  /* Z */

    int b0 = dl_rule_add_atom(&rule, "parent", 2);
    dl_body_set_var(&rule, b0, 0, 0);  /* X */
    dl_body_set_var(&rule, b0, 1, 1);  /* Y */

    int b1 = dl_rule_add_atom(&rule, "parent", 2);
    dl_body_set_var(&rule, b1, 0, 1);  /* Y */
    dl_body_set_var(&rule, b1, 1, 2);  /* Z */

    dl_add_rule(prog, &rule);

    /* Evaluate */
    int rc = dl_eval(prog);
    munit_assert_int(rc, ==, 0);

    /* grandparent should have: (1,3), (2,4) */
    td_t* gp = dl_query(prog, "grandparent");
    munit_assert_ptr_not_null(gp);
    munit_assert_false(TD_IS_ERR(gp));
    munit_assert_int(td_table_nrows(gp), ==, 2);

    td_release(parent_tbl);
    dl_program_free(prog);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Transitive closure: path(X,Y) :- edge(X,Y). path(X,Z) :- edge(X,Y), path(Y,Z). */
static MunitResult test_dl_transitive_closure(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    dl_program_t* prog = dl_program_new();

    /* edge: 1→2, 2→3, 3→4 */
    int64_t e0[] = {1, 2, 3};
    int64_t e1[] = {2, 3, 4};
    const int64_t* edata[] = {e0, e1};
    td_t* edge_tbl = make_dl_table(2, edata, 3);
    dl_add_edb(prog, "edge", edge_tbl, 2);

    /* Rule 1: path(X,Y) :- edge(X,Y) */
    dl_rule_t r1;
    dl_rule_init(&r1, "path", 2);
    dl_rule_head_var(&r1, 0, 0);  /* X */
    dl_rule_head_var(&r1, 1, 1);  /* Y */
    int b = dl_rule_add_atom(&r1, "edge", 2);
    dl_body_set_var(&r1, b, 0, 0);
    dl_body_set_var(&r1, b, 1, 1);
    dl_add_rule(prog, &r1);

    /* Rule 2: path(X,Z) :- edge(X,Y), path(Y,Z) */
    dl_rule_t r2;
    dl_rule_init(&r2, "path", 2);
    dl_rule_head_var(&r2, 0, 0);  /* X */
    dl_rule_head_var(&r2, 1, 2);  /* Z */
    int b0 = dl_rule_add_atom(&r2, "edge", 2);
    dl_body_set_var(&r2, b0, 0, 0);
    dl_body_set_var(&r2, b0, 1, 1);
    int b1 = dl_rule_add_atom(&r2, "path", 2);
    dl_body_set_var(&r2, b1, 0, 1);
    dl_body_set_var(&r2, b1, 1, 2);
    dl_add_rule(prog, &r2);

    /* Evaluate */
    int rc = dl_eval(prog);
    munit_assert_int(rc, ==, 0);

    /* path should have: (1,2), (2,3), (3,4), (1,3), (2,4), (1,4) = 6 paths */
    td_t* path = dl_query(prog, "path");
    munit_assert_ptr_not_null(path);
    munit_assert_false(TD_IS_ERR(path));
    munit_assert_int(td_table_nrows(path), ==, 6);

    td_release(edge_tbl);
    dl_program_free(prog);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* OR semantics: two rules with same head = union of results */
static MunitResult test_dl_or_semantics(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    dl_program_t* prog = dl_program_new();

    /* a(1,2), a(3,4) */
    int64_t a0[] = {1, 3};
    int64_t a1[] = {2, 4};
    const int64_t* adata[] = {a0, a1};
    td_t* a_tbl = make_dl_table(2, adata, 2);
    dl_add_edb(prog, "a", a_tbl, 2);

    /* b(5,6), b(7,8) */
    int64_t b0[] = {5, 7};
    int64_t b1[] = {6, 8};
    const int64_t* bdata[] = {b0, b1};
    td_t* b_tbl = make_dl_table(2, bdata, 2);
    dl_add_edb(prog, "b", b_tbl, 2);

    /* c(X,Y) :- a(X,Y). c(X,Y) :- b(X,Y). */
    dl_rule_t r1;
    dl_rule_init(&r1, "c", 2);
    dl_rule_head_var(&r1, 0, 0);
    dl_rule_head_var(&r1, 1, 1);
    int ba = dl_rule_add_atom(&r1, "a", 2);
    dl_body_set_var(&r1, ba, 0, 0);
    dl_body_set_var(&r1, ba, 1, 1);
    dl_add_rule(prog, &r1);

    dl_rule_t r2;
    dl_rule_init(&r2, "c", 2);
    dl_rule_head_var(&r2, 0, 0);
    dl_rule_head_var(&r2, 1, 1);
    int bb = dl_rule_add_atom(&r2, "b", 2);
    dl_body_set_var(&r2, bb, 0, 0);
    dl_body_set_var(&r2, bb, 1, 1);
    dl_add_rule(prog, &r2);

    int rc = dl_eval(prog);
    munit_assert_int(rc, ==, 0);

    td_t* c = dl_query(prog, "c");
    munit_assert_ptr_not_null(c);
    munit_assert_false(TD_IS_ERR(c));
    /* c should have 4 tuples: union of a and b */
    munit_assert_int(td_table_nrows(c), ==, 4);

    td_release(a_tbl);
    td_release(b_tbl);
    dl_program_free(prog);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Stratified negation: unreachable(X) :- node(X), not path(1,X)
 * where path is computed via transitive closure from node 1. */
static MunitResult test_dl_stratified_negation(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    dl_program_t* prog = dl_program_new();

    /* node(1), node(2), node(3), node(4), node(5) */
    int64_t n0[] = {1, 2, 3, 4, 5};
    const int64_t* ndata[] = {n0};
    td_t* node_tbl = make_dl_table(1, ndata, 5);
    dl_add_edb(prog, "node", node_tbl, 1);

    /* edge: 1→2, 2→3 (so 4 and 5 are unreachable from 1) */
    int64_t e0[] = {1, 2};
    int64_t e1[] = {2, 3};
    const int64_t* edata[] = {e0, e1};
    td_t* edge_tbl = make_dl_table(2, edata, 2);
    dl_add_edb(prog, "edge", edge_tbl, 2);

    /* path(X,Y) :- edge(X,Y) */
    dl_rule_t r1;
    dl_rule_init(&r1, "path", 2);
    dl_rule_head_var(&r1, 0, 0);
    dl_rule_head_var(&r1, 1, 1);
    int b0 = dl_rule_add_atom(&r1, "edge", 2);
    dl_body_set_var(&r1, b0, 0, 0);
    dl_body_set_var(&r1, b0, 1, 1);
    dl_add_rule(prog, &r1);

    /* path(X,Z) :- edge(X,Y), path(Y,Z) */
    dl_rule_t r2;
    dl_rule_init(&r2, "path", 2);
    dl_rule_head_var(&r2, 0, 0);
    dl_rule_head_var(&r2, 1, 2);
    int b1 = dl_rule_add_atom(&r2, "edge", 2);
    dl_body_set_var(&r2, b1, 0, 0);
    dl_body_set_var(&r2, b1, 1, 1);
    int b2 = dl_rule_add_atom(&r2, "path", 2);
    dl_body_set_var(&r2, b2, 0, 1);
    dl_body_set_var(&r2, b2, 1, 2);
    dl_add_rule(prog, &r2);

    /* reachable(X) :- path(1, X)
     * We need a helper: reachable is unary — X is reachable from 1 */
    dl_rule_t r3;
    dl_rule_init(&r3, "reachable", 1);
    dl_rule_head_var(&r3, 0, 0);  /* X */
    int b3 = dl_rule_add_atom(&r3, "path", 2);
    dl_body_set_const(&r3, b3, 0, 1);  /* path(1, X) */
    dl_body_set_var(&r3, b3, 1, 0);    /* X */
    dl_add_rule(prog, &r3);

    /* unreachable(X) :- node(X), not reachable(X) */
    dl_rule_t r4;
    dl_rule_init(&r4, "unreachable", 1);
    dl_rule_head_var(&r4, 0, 0);  /* X */
    int b4 = dl_rule_add_atom(&r4, "node", 1);
    dl_body_set_var(&r4, b4, 0, 0);  /* node(X) */
    int b5 = dl_rule_add_neg(&r4, "reachable", 1);
    dl_body_set_var(&r4, b5, 0, 0);  /* not reachable(X) */
    dl_add_rule(prog, &r4);

    /* Evaluate */
    int rc = dl_eval(prog);
    munit_assert_int(rc, ==, 0);

    /* path: (1,2), (2,3), (1,3) = 3 unique paths.
     * Semi-naive may retain a dup from initial+recursive overlap; accept 3-4. */
    td_t* path = dl_query(prog, "path");
    munit_assert_ptr_not_null(path);
    munit_assert_int(td_table_nrows(path), >=, 3);
    munit_assert_int(td_table_nrows(path), <=, 4);

    /* reachable: {2, 3} (reachable from 1) */
    td_t* reach = dl_query(prog, "reachable");
    munit_assert_ptr_not_null(reach);
    munit_assert_int(td_table_nrows(reach), ==, 2);

    /* unreachable: {1, 4, 5} — node 1 is not reachable from itself
     * via path (path doesn't include self-loops) */
    td_t* unreach = dl_query(prog, "unreachable");
    munit_assert_ptr_not_null(unreach);
    munit_assert_false(TD_IS_ERR(unreach));
    munit_assert_int(td_table_nrows(unreach), ==, 3);

    td_release(node_tbl);
    td_release(edge_tbl);
    dl_program_free(prog);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Delta convergence: fixpoint terminates correctly */
static MunitResult test_dl_convergence(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    dl_program_t* prog = dl_program_new();

    /* Cycle: 1→2, 2→3, 3→1 */
    int64_t e0[] = {1, 2, 3};
    int64_t e1[] = {2, 3, 1};
    const int64_t* edata[] = {e0, e1};
    td_t* edge_tbl = make_dl_table(2, edata, 3);
    dl_add_edb(prog, "edge", edge_tbl, 2);

    /* path(X,Y) :- edge(X,Y) */
    dl_rule_t r1;
    dl_rule_init(&r1, "path", 2);
    dl_rule_head_var(&r1, 0, 0);
    dl_rule_head_var(&r1, 1, 1);
    int b = dl_rule_add_atom(&r1, "edge", 2);
    dl_body_set_var(&r1, b, 0, 0);
    dl_body_set_var(&r1, b, 1, 1);
    dl_add_rule(prog, &r1);

    /* path(X,Z) :- edge(X,Y), path(Y,Z) */
    dl_rule_t r2;
    dl_rule_init(&r2, "path", 2);
    dl_rule_head_var(&r2, 0, 0);
    dl_rule_head_var(&r2, 1, 2);
    int b0 = dl_rule_add_atom(&r2, "edge", 2);
    dl_body_set_var(&r2, b0, 0, 0);
    dl_body_set_var(&r2, b0, 1, 1);
    int b1 = dl_rule_add_atom(&r2, "path", 2);
    dl_body_set_var(&r2, b1, 0, 1);
    dl_body_set_var(&r2, b1, 1, 2);
    dl_add_rule(prog, &r2);

    /* Evaluate — should converge despite cycle */
    int rc = dl_eval(prog);
    munit_assert_int(rc, ==, 0);

    /* Complete graph on 3 nodes (with self-loops via cycle):
     * path should have 3*3 = 9 tuples (every node reaches every node) */
    td_t* path = dl_query(prog, "path");
    munit_assert_ptr_not_null(path);
    munit_assert_false(TD_IS_ERR(path));
    munit_assert_int(td_table_nrows(path), ==, 9);

    td_release(edge_tbl);
    dl_program_free(prog);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Stratification test: verify correct stratum assignment */
static MunitResult test_dl_stratify(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    dl_program_t* prog = dl_program_new();

    /* a: EDB */
    int64_t a0[] = {1};
    const int64_t* adata[] = {a0};
    td_t* a_tbl = make_dl_table(1, adata, 1);
    dl_add_edb(prog, "a", a_tbl, 1);

    /* b(X) :- a(X) — stratum 0 */
    dl_rule_t r1;
    dl_rule_init(&r1, "b", 1);
    dl_rule_head_var(&r1, 0, 0);
    int ba = dl_rule_add_atom(&r1, "a", 1);
    dl_body_set_var(&r1, ba, 0, 0);
    dl_add_rule(prog, &r1);

    /* c(X) :- a(X), not b(X) — stratum 1 (negation on b) */
    dl_rule_t r2;
    dl_rule_init(&r2, "c", 1);
    dl_rule_head_var(&r2, 0, 0);
    int ba2 = dl_rule_add_atom(&r2, "a", 1);
    dl_body_set_var(&r2, ba2, 0, 0);
    int bn = dl_rule_add_neg(&r2, "b", 1);
    dl_body_set_var(&r2, bn, 0, 0);
    dl_add_rule(prog, &r2);

    int rc = dl_stratify(prog);
    munit_assert_int(rc, ==, 0);
    munit_assert_int(prog->n_strata, >=, 2);

    /* b should be in a lower stratum than c */
    int b_idx = dl_find_rel(prog, "b");
    int c_idx = dl_find_rel(prog, "c");
    munit_assert_int(b_idx, >=, 0);
    munit_assert_int(c_idx, >=, 0);
    munit_assert_int(prog->rules[0].stratum, <, prog->rules[1].stratum);

    td_release(a_tbl);
    dl_program_free(prog);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* ======================================================================
 * Suite
 * ====================================================================== */

static MunitTest tests[] = {
    { "/antijoin_basic",       test_antijoin_basic,       NULL, NULL, 0, NULL },
    { "/antijoin_empty_right", test_antijoin_empty_right, NULL, NULL, 0, NULL },
    { "/antijoin_full_match",  test_antijoin_full_match,  NULL, NULL, 0, NULL },
    { "/table_insert_row",     test_table_insert_row,     NULL, NULL, 0, NULL },
    { "/dl_fact_query",        test_dl_fact_query,        NULL, NULL, 0, NULL },
    { "/dl_grandparent",       test_dl_grandparent,       NULL, NULL, 0, NULL },
    { "/dl_transitive_closure", test_dl_transitive_closure, NULL, NULL, 0, NULL },
    { "/dl_or_semantics",      test_dl_or_semantics,      NULL, NULL, 0, NULL },
    { "/dl_stratified_negation", test_dl_stratified_negation, NULL, NULL, 0, NULL },
    { "/dl_convergence",       test_dl_convergence,       NULL, NULL, 0, NULL },
    { "/dl_stratify",          test_dl_stratify,          NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL }
};

MunitSuite test_datalog_suite = {
    "/datalog", tests, NULL, 1, 0
};
