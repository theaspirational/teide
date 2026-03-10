/*
 *   Copyright (c) 2024-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "munit.h"
#include <teide/td.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Build a 5-row table: id=[1,1,2,2,3], val=[10,20,30,40,50] */
static td_t* make_audit_table(void) {
    td_sym_init();

    int64_t id_data[]  = {1, 1, 2, 2, 3};
    int64_t val_data[] = {10, 20, 30, 40, 50};

    td_t* id_vec  = td_vec_from_raw(TD_I64, id_data, 5);
    td_t* val_vec = td_vec_from_raw(TD_I64, val_data, 5);

    int64_t name_id  = td_sym_intern("id", 2);
    int64_t name_val = td_sym_intern("val", 3);

    td_t* tbl = td_table_new(2);
    tbl = td_table_add_col(tbl, name_id, id_vec);
    tbl = td_table_add_col(tbl, name_val, val_vec);

    td_release(id_vec);
    td_release(val_vec);
    return tbl;
}

/* Build a 10-row table: id=[1,1,2,2,3,3,1,2,3,1], val=[10..100 step 10] */
static td_t* make_audit_table_10(void) {
    td_sym_init();

    int64_t id_data[]  = {1, 1, 2, 2, 3, 3, 1, 2, 3, 1};
    int64_t val_data[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

    td_t* id_vec  = td_vec_from_raw(TD_I64, id_data, 10);
    td_t* val_vec = td_vec_from_raw(TD_I64, val_data, 10);

    int64_t name_id  = td_sym_intern("id", 2);
    int64_t name_val = td_sym_intern("val", 3);

    td_t* tbl = td_table_new(2);
    tbl = td_table_add_col(tbl, name_id, id_vec);
    tbl = td_table_add_col(tbl, name_val, val_vec);

    td_release(id_vec);
    td_release(val_vec);
    return tbl;
}

/* Build a TD_SEL bitmap from a boolean array. */
static td_t* make_selection(const uint8_t* mask, int64_t n) {
    td_t* bvec = td_vec_from_raw(TD_BOOL, mask, n);
    td_t* sel = td_sel_from_pred(bvec);
    td_release(bvec);
    return sel;
}

/* -----------------------------------------------------------------------
 * Smoke test — basic table creation + SUM
 * ----------------------------------------------------------------------- */

static MunitResult test_audit_smoke(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* val = td_scan(g, "val");
    td_op_t* s   = td_sum(g, val);

    td_t* result = td_execute(g, s);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->i64, ==, 150);  /* 10+20+30+40+50 */

    td_release(result);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* -----------------------------------------------------------------------
 * Category 1: Selection bitmap + GROUP BY regression tests
 * ----------------------------------------------------------------------- */

/* Selection mask=[0,0,1,1,1] (rows where id>1) + GROUP BY id, SUM(val)
 * Expected: 2 groups — {id=2: sum=70, id=3: sum=50}, total=120 */
static MunitResult test_sel_group_sum(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);

    /* Set selection: rows 2,3,4 pass (id>1) */
    uint8_t mask[] = {0, 0, 1, 1, 1};
    td_t* sel = make_selection(mask, 5);
    munit_assert_false(TD_IS_ERR(sel));
    td_retain(sel);
    g->selection = sel;

    td_op_t* key = td_scan(g, "id");
    td_op_t* val = td_scan(g, "val");
    td_op_t* keys[] = { key };
    td_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    td_op_t* grp = td_group(g, keys, 1, agg_ops, agg_ins, 1);
    td_t* result = td_execute(g, grp);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);

    int64_t nrows = td_table_nrows(result);
    munit_assert_int(nrows, ==, 2);  /* groups: id=2, id=3 */

    /* Verify sums: id=2: 30+40=70, id=3: 50 */
    td_t* id_col  = td_table_get_col_idx(result, 0);
    td_t* sum_col = td_table_get_col_idx(result, 1);
    int64_t total = 0;
    int64_t sum2 = 0, sum3 = 0;
    for (int64_t i = 0; i < nrows; i++) {
        int64_t id = ((int64_t*)td_data(id_col))[i];
        int64_t s  = ((int64_t*)td_data(sum_col))[i];
        total += s;
        if (id == 2) sum2 = s;
        else if (id == 3) sum3 = s;
    }
    munit_assert_int(total, ==, 120);
    munit_assert_int(sum2, ==, 70);
    munit_assert_int(sum3, ==, 50);

    td_release(result);
    td_release(sel);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Same selection + GROUP BY id, COUNT(val) → 2 groups */
static MunitResult test_sel_group_count(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);

    uint8_t mask[] = {0, 0, 1, 1, 1};
    td_t* sel = make_selection(mask, 5);
    munit_assert_false(TD_IS_ERR(sel));
    td_retain(sel);
    g->selection = sel;

    td_op_t* key = td_scan(g, "id");
    td_op_t* val = td_scan(g, "val");
    td_op_t* keys[] = { key };
    td_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_COUNT };

    td_op_t* grp = td_group(g, keys, 1, agg_ops, agg_ins, 1);
    td_t* result = td_execute(g, grp);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);

    int64_t nrows = td_table_nrows(result);
    munit_assert_int(nrows, ==, 2);  /* groups: id=2, id=3 */

    /* Verify counts: id=2: 2 rows, id=3: 1 row */
    td_t* id_col  = td_table_get_col_idx(result, 0);
    td_t* cnt_col = td_table_get_col_idx(result, 1);
    int64_t cnt2 = 0, cnt3 = 0;
    for (int64_t i = 0; i < nrows; i++) {
        int64_t id = ((int64_t*)td_data(id_col))[i];
        int64_t c  = ((int64_t*)td_data(cnt_col))[i];
        if (id == 2) cnt2 = c;
        else if (id == 3) cnt3 = c;
    }
    munit_assert_int(cnt2, ==, 2);
    munit_assert_int(cnt3, ==, 1);

    td_release(result);
    td_release(sel);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Selection + scalar SUM(val) (no GROUP BY keys) → 120 */
static MunitResult test_sel_group_scalar(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);

    uint8_t mask[] = {0, 0, 1, 1, 1};
    td_t* sel = make_selection(mask, 5);
    munit_assert_false(TD_IS_ERR(sel));
    td_retain(sel);
    g->selection = sel;

    td_op_t* val = td_scan(g, "val");
    uint16_t agg_ops[] = { OP_SUM };
    td_op_t* agg_ins[] = { val };

    td_op_t* grp = td_group(g, NULL, 0, agg_ops, agg_ins, 1);
    td_t* result = td_execute(g, grp);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);

    int64_t nrows = td_table_nrows(result);
    munit_assert_int(nrows, ==, 1);

    td_t* sum_col = td_table_get_col_idx(result, 0);
    munit_assert_int(((int64_t*)td_data(sum_col))[0], ==, 120);

    td_release(result);
    td_release(sel);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* -----------------------------------------------------------------------
 * Category 1: Selection bitmap + SORT regression tests
 * ----------------------------------------------------------------------- */

/* Selection mask=[1,0,1,0,1] (rows 0,2,4) + SORT by val DESC
 * Expected: [50,30,10], nrows==3 */
static MunitResult test_sel_sort(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);

    uint8_t mask[] = {1, 0, 1, 0, 1};
    td_t* sel = make_selection(mask, 5);
    munit_assert_false(TD_IS_ERR(sel));
    td_retain(sel);
    g->selection = sel;

    td_op_t* tbl_op = td_const_table(g, tbl);
    td_op_t* val_key = td_scan(g, "val");
    td_op_t* keys[] = { val_key };
    uint8_t descs[] = { 1 };           /* descending */
    uint8_t nulls_first[] = { 0 };
    td_op_t* sort_op = td_sort_op(g, tbl_op, keys, descs, nulls_first, 1);

    td_t* result = td_execute(g, sort_op);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);

    /* Verify descending order: [50, 30, 10] */
    int64_t name_val = td_sym_intern("val", 3);
    td_t* val_col = td_table_get_col(result, name_val);
    munit_assert_ptr_not_null(val_col);
    int64_t* vdata = (int64_t*)td_data(val_col);
    munit_assert_int(vdata[0], ==, 50);
    munit_assert_int(vdata[1], ==, 30);
    munit_assert_int(vdata[2], ==, 10);

    td_release(result);
    td_release(sel);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* The full reported bug chain: selection + GROUP BY + SORT.
 * Table 10 rows: id=[1,1,2,2,3,3,1,2,3,1], val=[10..100].
 * Selection: mask=[0,0,1,1,1,1,0,1,1,0] (id>1 → 6 rows pass).
 * GROUP BY id, SUM(val) → {2: 30+40+80=150, 3: 50+60+90=200}.
 * SORT by id ASC → [(2,150), (3,200)].
 * Verify nrows==2, ids=[2,3], vals=[150,200]. */
static MunitResult test_sel_group_sort(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table_10();

    td_graph_t* g = td_graph_new(tbl);

    uint8_t mask[] = {0, 0, 1, 1, 1, 1, 0, 1, 1, 0};
    td_t* sel = make_selection(mask, 10);
    munit_assert_false(TD_IS_ERR(sel));
    td_retain(sel);
    g->selection = sel;

    /* GROUP BY id, SUM(val) */
    td_op_t* key = td_scan(g, "id");
    td_op_t* val = td_scan(g, "val");
    td_op_t* grp_keys[] = { key };
    td_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    td_op_t* grp = td_group(g, grp_keys, 1, agg_ops, agg_ins, 1);

    /* SORT by id ASC — use group result as table input */
    td_op_t* sort_keys[] = { key };
    uint8_t descs[] = { 0 };           /* ascending */
    uint8_t nulls_first[] = { 0 };
    td_op_t* sort_op = td_sort_op(g, grp, sort_keys, descs, nulls_first, 1);

    td_t* result = td_execute(g, sort_op);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);

    int64_t nrows = td_table_nrows(result);
    munit_assert_int(nrows, ==, 2);

    /* Verify sorted: id=[2,3], sum=[150,200] */
    td_t* id_col  = td_table_get_col_idx(result, 0);
    td_t* sum_col = td_table_get_col_idx(result, 1);
    munit_assert_ptr_not_null(id_col);
    munit_assert_ptr_not_null(sum_col);

    int64_t* ids  = (int64_t*)td_data(id_col);
    int64_t* sums = (int64_t*)td_data(sum_col);
    munit_assert_int(ids[0], ==, 2);
    munit_assert_int(ids[1], ==, 3);
    munit_assert_int(sums[0], ==, 150);
    munit_assert_int(sums[1], ==, 200);

    td_release(result);
    td_release(sel);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* -----------------------------------------------------------------------
 * Test array + suite registration
 * ----------------------------------------------------------------------- */

static MunitTest audit_tests[] = {
    { "/smoke",            test_audit_smoke,       NULL, NULL, 0, NULL },
    { "/sel_group_sum",    test_sel_group_sum,     NULL, NULL, 0, NULL },
    { "/sel_group_count",  test_sel_group_count,   NULL, NULL, 0, NULL },
    { "/sel_group_scalar", test_sel_group_scalar,  NULL, NULL, 0, NULL },
    { "/sel_sort",         test_sel_sort,          NULL, NULL, 0, NULL },
    { "/sel_group_sort",   test_sel_group_sort,    NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL }
};

MunitSuite test_audit_suite = {
    "/audit", audit_tests, NULL, 1, 0
};
