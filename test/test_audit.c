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
 * Category 1: Selection bitmap + JOIN / WINDOW / ASOF
 * ----------------------------------------------------------------------- */

/* Selection on left table + INNER JOIN on id.
 * Left: make_audit_table() (id=[1,1,2,2,3], val=[10,20,30,40,50]).
 * Right: id=[2,3], label=[200,300].
 * Selection: mask=[0,0,1,1,1] (rows 2,3,4).
 * INNER JOIN on id → 3 rows (id=2 twice, id=3 once). */
static MunitResult test_sel_join(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* left = make_audit_table();

    /* Build right table: id=[2,3], label=[200,300] */
    int64_t rid_data[]   = {2, 3};
    int64_t rlabel_data[] = {200, 300};
    td_t* rid_vec   = td_vec_from_raw(TD_I64, rid_data, 2);
    td_t* rlabel_vec = td_vec_from_raw(TD_I64, rlabel_data, 2);
    int64_t name_id    = td_sym_intern("id", 2);
    int64_t name_label = td_sym_intern("label", 5);
    td_t* right = td_table_new(2);
    right = td_table_add_col(right, name_id, rid_vec);
    right = td_table_add_col(right, name_label, rlabel_vec);
    td_release(rid_vec);
    td_release(rlabel_vec);

    td_graph_t* g = td_graph_new(left);

    uint8_t mask[] = {0, 0, 1, 1, 1};
    td_t* sel = make_selection(mask, 5);
    munit_assert_false(TD_IS_ERR(sel));
    td_retain(sel);
    g->selection = sel;

    td_op_t* left_op  = td_const_table(g, left);
    td_op_t* right_op = td_const_table(g, right);
    td_op_t* lk = td_scan(g, "id");
    td_op_t* lk_arr[] = { lk };
    td_op_t* rk_arr[] = { lk };
    td_op_t* join_op = td_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    td_t* result = td_execute(g, join_op);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);

    td_release(result);
    td_release(sel);
    td_release(right);
    td_graph_free(g);
    td_release(left);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Selection + WINDOW ROW_NUMBER() OVER (ORDER BY val ASC).
 * Table: make_audit_table(). Selection: mask=[1,1,1,0,0] (3 rows).
 * Result: nrows==3, with row numbers 1,2,3. */
static MunitResult test_sel_window(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);

    uint8_t mask[] = {1, 1, 1, 0, 0};
    td_t* sel = make_selection(mask, 5);
    munit_assert_false(TD_IS_ERR(sel));
    td_retain(sel);
    g->selection = sel;

    td_op_t* tbl_op = td_const_table(g, tbl);
    td_op_t* val_op = td_scan(g, "val");
    td_op_t* orders[] = { val_op };
    uint8_t order_descs[] = { 0 };
    uint8_t func_kinds[] = { TD_WIN_ROW_NUMBER };
    td_op_t* func_inputs[] = { val_op };
    int64_t func_params[] = { 0 };
    td_op_t* win = td_window_op(g, tbl_op,
                                NULL, 0,
                                orders, order_descs, 1,
                                func_kinds, func_inputs, func_params, 1,
                                TD_FRAME_ROWS,
                                TD_BOUND_UNBOUNDED_PRECEDING,
                                TD_BOUND_UNBOUNDED_FOLLOWING,
                                0, 0);

    td_t* result = td_execute(g, win);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);

    td_release(result);
    td_release(sel);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Selection + inner ASOF JOIN.
 * Left: time=[100,200,300,400,500], val=[1.0..5.0]. Selection: [1,1,0,0,1].
 * Right: time=[90,150,450], bid=[0.9,1.5,4.5].
 * Inner ASOF → nrows==3. */
static MunitResult test_sel_asof_join(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    /* Build left table: time(I64), val(F64) */
    int64_t ltime_data[] = {100, 200, 300, 400, 500};
    double  lval_data[]  = {1.0, 2.0, 3.0, 4.0, 5.0};
    td_t* ltime_vec = td_vec_from_raw(TD_I64, ltime_data, 5);
    td_t* lval_vec  = td_vec_from_raw(TD_F64, lval_data, 5);
    int64_t name_time = td_sym_intern("time", 4);
    int64_t name_val  = td_sym_intern("val", 3);
    td_t* left = td_table_new(2);
    left = td_table_add_col(left, name_time, ltime_vec);
    left = td_table_add_col(left, name_val, lval_vec);
    td_release(ltime_vec);
    td_release(lval_vec);

    /* Build right table: time(I64), bid(F64) */
    int64_t rtime_data[] = {90, 150, 450};
    double  rbid_data[]  = {0.9, 1.5, 4.5};
    td_t* rtime_vec = td_vec_from_raw(TD_I64, rtime_data, 3);
    td_t* rbid_vec  = td_vec_from_raw(TD_F64, rbid_data, 3);
    int64_t name_bid = td_sym_intern("bid", 3);
    td_t* right = td_table_new(2);
    right = td_table_add_col(right, name_time, rtime_vec);
    right = td_table_add_col(right, name_bid, rbid_vec);
    td_release(rtime_vec);
    td_release(rbid_vec);

    td_graph_t* g = td_graph_new(left);

    uint8_t mask[] = {1, 1, 0, 0, 1};
    td_t* sel = make_selection(mask, 5);
    munit_assert_false(TD_IS_ERR(sel));
    td_retain(sel);
    g->selection = sel;

    td_op_t* left_op  = td_const_table(g, left);
    td_op_t* right_op = td_const_table(g, right);
    td_op_t* tkey = td_scan(g, "time");
    td_op_t* aj = td_asof_join(g, left_op, right_op, tkey, NULL, 0, 0);

    td_t* result = td_execute(g, aj);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);

    td_release(result);
    td_release(sel);
    td_release(right);
    td_graph_free(g);
    td_release(left);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* -----------------------------------------------------------------------
 * Category 1: Chained selection + boundary cases
 * ----------------------------------------------------------------------- */

/* Two selections ANDed: mask_a=[0,0,1,1,1,1,0,1,1,0], mask_b=[1,1,1,1,1,1,0,0,0,0].
 * AND → [0,0,1,1,1,1,0,0,0,0] → rows 2,3,4,5.
 * GROUP BY id, SUM(val) → 2 groups {2:70, 3:110}. */
static MunitResult test_sel_chained(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table_10();

    td_graph_t* g = td_graph_new(tbl);

    uint8_t mask_a[] = {0, 0, 1, 1, 1, 1, 0, 1, 1, 0};
    uint8_t mask_b[] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0};
    td_t* sel_a = make_selection(mask_a, 10);
    td_t* sel_b = make_selection(mask_b, 10);
    td_t* sel_and = td_sel_and(sel_a, sel_b);
    munit_assert_false(TD_IS_ERR(sel_and));
    td_retain(sel_and);
    g->selection = sel_and;
    td_release(sel_a);
    td_release(sel_b);

    td_op_t* key = td_scan(g, "id");
    td_op_t* val = td_scan(g, "val");
    td_op_t* keys[] = { key };
    td_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    td_op_t* grp = td_group(g, keys, 1, agg_ops, agg_ins, 1);
    td_t* result = td_execute(g, grp);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 2);

    td_release(result);
    td_release(sel_and);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* All rows filtered out (mask all zeros). GROUP BY → 0-row result. */
static MunitResult test_sel_all_filtered(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);

    uint8_t mask[] = {0, 0, 0, 0, 0};
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
    munit_assert_int(td_table_nrows(result), ==, 0);

    td_release(result);
    td_release(sel);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* All rows pass (mask all ones). GROUP BY id, SUM(val) → 3 groups, total=150. */
static MunitResult test_sel_none_filtered(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);

    uint8_t mask[] = {1, 1, 1, 1, 1};
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
    munit_assert_int(td_table_nrows(result), ==, 3);

    /* Sum all values across groups */
    td_t* sum_col = td_table_get_col_idx(result, 1);
    int64_t total = 0;
    for (int64_t i = 0; i < 3; i++) {
        total += ((int64_t*)td_data(sum_col))[i];
    }
    munit_assert_int(total, ==, 150);

    td_release(result);
    td_release(sel);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* -----------------------------------------------------------------------
 * Category 1: FILTER lazy/eager + HAVING fusion
 * ----------------------------------------------------------------------- */

/* FILTER(table, id>1) → lazy path creates TD_SEL. Verify nrows==3. */
static MunitResult test_filter_lazy(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* tbl_op = td_const_table(g, tbl);
    td_op_t* id_col = td_scan(g, "id");
    td_op_t* one = td_const_i64(g, 1);
    td_op_t* pred = td_gt(g, id_col, one);
    td_op_t* filt = td_filter(g, tbl_op, pred);

    td_t* result = td_execute(g, filt);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);

    td_release(result);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Chained FILTER: FILTER(FILTER(table, id>1), val<50).
 * id>1: rows (2,30),(2,40),(3,50). val<50: (2,30),(2,40) → nrows==2. */
static MunitResult test_filter_lazy_chained(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* tbl_op = td_const_table(g, tbl);
    td_op_t* id_col = td_scan(g, "id");
    td_op_t* val_col = td_scan(g, "val");
    td_op_t* one = td_const_i64(g, 1);
    td_op_t* fifty = td_const_i64(g, 50);
    td_op_t* pred1 = td_gt(g, id_col, one);
    td_op_t* filt1 = td_filter(g, tbl_op, pred1);
    td_op_t* pred2 = td_lt(g, val_col, fifty);
    td_op_t* filt2 = td_filter(g, filt1, pred2);

    td_t* result = td_execute(g, filt2);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 2);

    td_release(result);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* HAVING fusion: GROUP BY id, SUM(val) → FILTER(GROUP, sum>40).
 * Groups: {1:30, 2:70, 3:50}. HAVING >40 → {2:70, 3:50} → nrows==2. */
static MunitResult test_having_fusion(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* key = td_scan(g, "id");
    td_op_t* val = td_scan(g, "val");
    td_op_t* keys[] = { key };
    td_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    td_op_t* grp = td_group(g, keys, 1, agg_ops, agg_ins, 1);
    td_op_t* forty = td_const_i64(g, 40);
    /* After GROUP BY, SUM(val) column is named "val_sum" */
    td_op_t* sum_scan = td_scan(g, "val_sum");
    td_op_t* pred = td_gt(g, sum_scan, forty);
    td_op_t* having = td_filter(g, grp, pred);

    td_t* result = td_execute(g, having);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 2);

    td_release(result);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* HAVING with selection: 10-row table + selection + GROUP + HAVING.
 * Selection: mask=[0,0,1,1,1,1,0,1,1,0] → rows 2..5,7,8.
 * id=[2,2,3,3,2,3], val=[30,40,50,60,80,90].
 * GROUP BY id, SUM(val) → {2: 30+40+80=150, 3: 50+60+90=200}.
 * HAVING sum>160 → {3:200} → nrows==1. */
static MunitResult test_having_with_selection(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table_10();

    td_graph_t* g = td_graph_new(tbl);

    uint8_t mask[] = {0, 0, 1, 1, 1, 1, 0, 1, 1, 0};
    td_t* sel = make_selection(mask, 10);
    munit_assert_false(TD_IS_ERR(sel));
    td_retain(sel);
    g->selection = sel;

    td_op_t* key = td_scan(g, "id");
    td_op_t* val = td_scan(g, "val");
    td_op_t* keys[] = { key };
    td_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    td_op_t* grp = td_group(g, keys, 1, agg_ops, agg_ins, 1);
    td_op_t* threshold = td_const_i64(g, 160);
    /* After GROUP BY, SUM(val) column is named "val_sum" */
    td_op_t* sum_scan = td_scan(g, "val_sum");
    td_op_t* pred = td_gt(g, sum_scan, threshold);
    td_op_t* having = td_filter(g, grp, pred);

    td_t* result = td_execute(g, having);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 1);

    td_release(result);
    td_release(sel);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* -----------------------------------------------------------------------
 * Category 2: GROUP BY fallback path tests (no selection bitmap)
 * ----------------------------------------------------------------------- */

/* Small table GROUP BY (sequential path). 3 groups, total sum = 150. */
static MunitResult test_group_small(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* key = td_scan(g, "id");
    td_op_t* val = td_scan(g, "val");
    td_op_t* keys[] = { key };
    td_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    td_op_t* grp = td_group(g, keys, 1, agg_ops, agg_ins, 1);
    td_t* result = td_execute(g, grp);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);

    /* Verify total sum across all groups is 150 */
    td_t* sum_col = td_table_get_col_idx(result, 1);
    int64_t total = 0;
    for (int64_t i = 0; i < 3; i++) {
        total += ((int64_t*)td_data(sum_col))[i];
    }
    munit_assert_int(total, ==, 150);

    td_release(result);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* GROUP BY with 4 aggregates: SUM, COUNT, MIN, MAX. ncols==5, nrows==3. */
static MunitResult test_group_multi_agg(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* key = td_scan(g, "id");
    td_op_t* v1 = td_scan(g, "val");
    td_op_t* v2 = td_scan(g, "val");
    td_op_t* v3 = td_scan(g, "val");
    td_op_t* v4 = td_scan(g, "val");
    td_op_t* keys[] = { key };
    td_op_t* agg_ins[] = { v1, v2, v3, v4 };
    uint16_t agg_ops[] = { OP_SUM, OP_COUNT, OP_MIN, OP_MAX };

    td_op_t* grp = td_group(g, keys, 1, agg_ops, agg_ins, 4);
    td_t* result = td_execute(g, grp);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 3);
    munit_assert_int(td_table_ncols(result), ==, 5);  /* id + 4 aggs */

    td_release(result);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* GROUP BY val (each row unique group). COUNT(val). nrows==5. */
static MunitResult test_group_single_row_groups(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* val_key = td_scan(g, "val");
    td_op_t* val_agg = td_scan(g, "val");
    td_op_t* keys[] = { val_key };
    td_op_t* agg_ins[] = { val_agg };
    uint16_t agg_ops[] = { OP_COUNT };

    td_op_t* grp = td_group(g, keys, 1, agg_ops, agg_ins, 1);
    td_t* result = td_execute(g, grp);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 5);

    td_release(result);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* -----------------------------------------------------------------------
 * Category 2: SORT / JOIN / FILTER fallback tests (no selection bitmap)
 * ----------------------------------------------------------------------- */

/* Multi-column sort: SORT BY id DESC, val ASC.
 * Expected: ids=[3,2,2,1,1], within id=2 vals=[30,40], within id=1 vals=[10,20]. */
static MunitResult test_sort_small(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* tbl_op = td_const_table(g, tbl);
    td_op_t* k1 = td_scan(g, "id");
    td_op_t* k2 = td_scan(g, "val");
    td_op_t* keys[] = { k1, k2 };
    uint8_t descs[] = { 1, 0 };        /* id DESC, val ASC */
    uint8_t nulls_first[] = { 0, 0 };
    td_op_t* sort_op = td_sort_op(g, tbl_op, keys, descs, nulls_first, 2);

    td_t* result = td_execute(g, sort_op);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 5);

    int64_t name_id  = td_sym_intern("id", 2);
    int64_t name_val = td_sym_intern("val", 3);
    td_t* id_col  = td_table_get_col(result, name_id);
    td_t* val_col = td_table_get_col(result, name_val);
    int64_t* ids  = (int64_t*)td_data(id_col);
    int64_t* vals = (int64_t*)td_data(val_col);

    /* ids should be [3,2,2,1,1] */
    munit_assert_int(ids[0], ==, 3);
    munit_assert_int(ids[1], ==, 2);
    munit_assert_int(ids[2], ==, 2);
    munit_assert_int(ids[3], ==, 1);
    munit_assert_int(ids[4], ==, 1);

    /* within id=2: vals=[30,40] (ASC) */
    munit_assert_int(vals[1], ==, 30);
    munit_assert_int(vals[2], ==, 40);

    /* within id=1: vals=[10,20] (ASC) */
    munit_assert_int(vals[3], ==, 10);
    munit_assert_int(vals[4], ==, 20);

    td_release(result);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* INNER JOIN: Left=make_audit_table(), Right: id=[1,2], x=[100,200].
 * id=1 matches 2 left rows, id=2 matches 2 left rows → 4 result rows. */
static MunitResult test_join_small(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* left = make_audit_table();

    /* Build right table: id=[1,2], x=[100,200] */
    int64_t rid_data[] = {1, 2};
    int64_t rx_data[]  = {100, 200};
    td_t* rid_vec = td_vec_from_raw(TD_I64, rid_data, 2);
    td_t* rx_vec  = td_vec_from_raw(TD_I64, rx_data, 2);
    int64_t name_id = td_sym_intern("id", 2);
    int64_t name_x  = td_sym_intern("x", 1);
    td_t* right = td_table_new(2);
    right = td_table_add_col(right, name_id, rid_vec);
    right = td_table_add_col(right, name_x, rx_vec);
    td_release(rid_vec);
    td_release(rx_vec);

    td_graph_t* g = td_graph_new(left);
    td_op_t* left_op  = td_const_table(g, left);
    td_op_t* right_op = td_const_table(g, right);
    td_op_t* lk = td_scan(g, "id");
    td_op_t* lk_arr[] = { lk };
    td_op_t* rk_arr[] = { lk };
    td_op_t* join_op = td_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    td_t* result = td_execute(g, join_op);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->type, ==, TD_TABLE);
    munit_assert_int(td_table_nrows(result), ==, 4);

    td_release(result);
    td_release(right);
    td_graph_free(g);
    td_release(left);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Filter on vector input (eager path). val > 30 → 2 rows (40, 50). */
static MunitResult test_filter_eager(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* tbl = make_audit_table();

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* val = td_scan(g, "val");
    td_op_t* threshold = td_const_i64(g, 30);
    td_op_t* pred = td_gt(g, val, threshold);
    td_op_t* filtered = td_filter(g, val, pred);
    td_op_t* cnt = td_count(g, filtered);

    td_t* result = td_execute(g, cnt);
    munit_assert_false(TD_IS_ERR(result));
    munit_assert_int(result->i64, ==, 2);

    td_release(result);
    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* -----------------------------------------------------------------------
 * Category 3: Error path edge cases (empty tables)
 * ----------------------------------------------------------------------- */

/* Empty table (0 rows): GROUP BY id, SUM(val) → should not crash. */
static MunitResult test_group_empty_table(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    td_t* id_vec  = td_vec_new(TD_I64, 0);
    td_t* val_vec = td_vec_new(TD_I64, 0);
    int64_t name_id  = td_sym_intern("id", 2);
    int64_t name_val = td_sym_intern("val", 3);
    td_t* tbl = td_table_new(2);
    tbl = td_table_add_col(tbl, name_id, id_vec);
    tbl = td_table_add_col(tbl, name_val, val_vec);
    td_release(id_vec);
    td_release(val_vec);

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* key = td_scan(g, "id");
    td_op_t* val = td_scan(g, "val");
    td_op_t* keys[] = { key };
    td_op_t* agg_ins[] = { val };
    uint16_t agg_ops[] = { OP_SUM };

    td_op_t* grp = td_group(g, keys, 1, agg_ops, agg_ins, 1);
    td_t* result = td_execute(g, grp);
    /* Accept either empty result or error — just must not crash */
    if (result && !TD_IS_ERR(result)) {
        td_release(result);
    }

    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Empty 1-column table. SORT by val ASC → 0-row result, no crash. */
static MunitResult test_sort_empty_table(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    td_t* val_vec = td_vec_new(TD_I64, 0);
    int64_t name_val = td_sym_intern("val", 3);
    td_t* tbl = td_table_new(1);
    tbl = td_table_add_col(tbl, name_val, val_vec);
    td_release(val_vec);

    td_graph_t* g = td_graph_new(tbl);
    td_op_t* tbl_op = td_const_table(g, tbl);
    td_op_t* val_key = td_scan(g, "val");
    td_op_t* keys[] = { val_key };
    uint8_t descs[] = { 0 };
    uint8_t nulls_first[] = { 0 };
    td_op_t* sort_op = td_sort_op(g, tbl_op, keys, descs, nulls_first, 1);

    td_t* result = td_execute(g, sort_op);
    /* Accept either empty result or error — just must not crash */
    if (result && !TD_IS_ERR(result)) {
        munit_assert_int(td_table_nrows(result), ==, 0);
        td_release(result);
    }

    td_graph_free(g);
    td_release(tbl);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* Left: make_audit_table(). Right: empty table (0 rows with id column).
 * INNER JOIN → 0 result rows. */
static MunitResult test_join_empty_table(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_t* left = make_audit_table();

    /* Build empty right table with "id" column */
    td_t* rid_vec = td_vec_new(TD_I64, 0);
    int64_t name_id = td_sym_intern("id", 2);
    td_t* right = td_table_new(1);
    right = td_table_add_col(right, name_id, rid_vec);
    td_release(rid_vec);

    td_graph_t* g = td_graph_new(left);
    td_op_t* left_op  = td_const_table(g, left);
    td_op_t* right_op = td_const_table(g, right);
    td_op_t* lk = td_scan(g, "id");
    td_op_t* lk_arr[] = { lk };
    td_op_t* rk_arr[] = { lk };
    td_op_t* join_op = td_join(g, left_op, lk_arr, right_op, rk_arr, 1, 0);

    td_t* result = td_execute(g, join_op);
    /* Accept either empty result or error — just must not crash */
    if (result && !TD_IS_ERR(result)) {
        munit_assert_int(td_table_nrows(result), ==, 0);
        td_release(result);
    }

    td_release(right);
    td_graph_free(g);
    td_release(left);
    td_sym_destroy();
    td_heap_destroy();
    return MUNIT_OK;
}

/* -----------------------------------------------------------------------
 * Test array + suite registration
 * ----------------------------------------------------------------------- */

static MunitTest audit_tests[] = {
    { "/smoke",                  test_audit_smoke,            NULL, NULL, 0, NULL },
    { "/sel_group_sum",          test_sel_group_sum,          NULL, NULL, 0, NULL },
    { "/sel_group_count",        test_sel_group_count,        NULL, NULL, 0, NULL },
    { "/sel_group_scalar",       test_sel_group_scalar,       NULL, NULL, 0, NULL },
    { "/sel_sort",               test_sel_sort,               NULL, NULL, 0, NULL },
    { "/sel_group_sort",         test_sel_group_sort,         NULL, NULL, 0, NULL },
    /* Task 4: Selection + JOIN / WINDOW / ASOF */
    { "/sel_join",               test_sel_join,               NULL, NULL, 0, NULL },
    { "/sel_window",             test_sel_window,             NULL, NULL, 0, NULL },
    { "/sel_asof_join",          test_sel_asof_join,          NULL, NULL, 0, NULL },
    /* Task 5: Chained selection + boundary cases */
    { "/sel_chained",            test_sel_chained,            NULL, NULL, 0, NULL },
    { "/sel_all_filtered",       test_sel_all_filtered,       NULL, NULL, 0, NULL },
    { "/sel_none_filtered",      test_sel_none_filtered,      NULL, NULL, 0, NULL },
    /* Task 6: FILTER lazy/eager + HAVING fusion */
    { "/filter_lazy",            test_filter_lazy,            NULL, NULL, 0, NULL },
    { "/filter_lazy_chained",    test_filter_lazy_chained,    NULL, NULL, 0, NULL },
    { "/having_fusion",          test_having_fusion,          NULL, NULL, 0, NULL },
    { "/having_with_selection",  test_having_with_selection,  NULL, NULL, 0, NULL },
    /* Task 7: Category 2 — GROUP BY fallback path tests */
    { "/group_small",            test_group_small,            NULL, NULL, 0, NULL },
    { "/group_multi_agg",        test_group_multi_agg,        NULL, NULL, 0, NULL },
    { "/group_single_row_groups",test_group_single_row_groups,NULL, NULL, 0, NULL },
    /* Task 8: Category 2 — SORT/JOIN/FILTER fallback tests */
    { "/sort_small",             test_sort_small,             NULL, NULL, 0, NULL },
    { "/join_small",             test_join_small,             NULL, NULL, 0, NULL },
    { "/filter_eager",           test_filter_eager,           NULL, NULL, 0, NULL },
    /* Task 9: Category 3 — Error path edge cases (empty tables) */
    { "/group_empty_table",      test_group_empty_table,      NULL, NULL, 0, NULL },
    { "/sort_empty_table",       test_sort_empty_table,       NULL, NULL, 0, NULL },
    { "/join_empty_table",       test_join_empty_table,       NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL }
};

MunitSuite test_audit_suite = {
    "/audit", audit_tests, NULL, 1, 0
};
