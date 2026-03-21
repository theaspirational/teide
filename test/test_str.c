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

/* ---- Setup / Teardown -------------------------------------------------- */

static void* str_setup(const void* params, void* user_data) {
    (void)params; (void)user_data;
    td_heap_init();
    return NULL;
}

static void str_teardown(void* fixture) {
    (void)fixture;
    td_heap_destroy();
}

/* ---- str_ptr SSO ------------------------------------------------------- */

static MunitResult test_str_ptr_sso(const void* params, void* fixture) {
    (void)params; (void)fixture;

    td_t* s = td_str("hello", 5);
    munit_assert_ptr_not_null(s);

    const char* p = td_str_ptr(s);
    munit_assert_ptr_not_null(p);
    munit_assert_memory_equal(5, p, "hello");

    td_release(s);
    return MUNIT_OK;
}

/* ---- str_ptr long ------------------------------------------------------ */

static MunitResult test_str_ptr_long(const void* params, void* fixture) {
    (void)params; (void)fixture;

    const char* text = "this is a longer string";
    size_t len = strlen(text);
    td_t* s = td_str(text, len);
    munit_assert_ptr_not_null(s);

    const char* p = td_str_ptr(s);
    munit_assert_ptr_not_null(p);
    munit_assert_memory_equal(len, p, text);

    td_release(s);
    return MUNIT_OK;
}

/* ---- str_len ----------------------------------------------------------- */

static MunitResult test_str_len(const void* params, void* fixture) {
    (void)params; (void)fixture;

    /* SSO */
    td_t* s1 = td_str("abc", 3);
    munit_assert_size(td_str_len(s1), ==, 3);
    td_release(s1);

    /* Empty SSO */
    td_t* s2 = td_str("", 0);
    munit_assert_size(td_str_len(s2), ==, 0);
    td_release(s2);

    /* Long */
    const char* text = "a longer string for testing";
    size_t len = strlen(text);
    td_t* s3 = td_str(text, len);
    munit_assert_size(td_str_len(s3), ==, len);
    td_release(s3);

    return MUNIT_OK;
}

/* ---- str_cmp equal ----------------------------------------------------- */

static MunitResult test_str_cmp_equal(const void* params, void* fixture) {
    (void)params; (void)fixture;

    td_t* a = td_str("hello", 5);
    td_t* b = td_str("hello", 5);

    munit_assert_int(td_str_cmp(a, b), ==, 0);

    td_release(a);
    td_release(b);
    return MUNIT_OK;
}

/* ---- str_cmp different ------------------------------------------------- */

static MunitResult test_str_cmp_different(const void* params, void* fixture) {
    (void)params; (void)fixture;

    td_t* a = td_str("abc", 3);
    td_t* b = td_str("abd", 3);

    int cmp = td_str_cmp(a, b);
    munit_assert_int(cmp, <, 0);  /* 'c' < 'd' */

    int cmp2 = td_str_cmp(b, a);
    munit_assert_int(cmp2, >, 0);

    td_release(a);
    td_release(b);
    return MUNIT_OK;
}

/* ---- str_cmp prefix ---------------------------------------------------- */

static MunitResult test_str_cmp_prefix(const void* params, void* fixture) {
    (void)params; (void)fixture;

    td_t* a = td_str("abc", 3);
    td_t* b = td_str("abcde", 5);

    int cmp = td_str_cmp(a, b);
    munit_assert_int(cmp, <, 0);  /* shorter sorts first */

    int cmp2 = td_str_cmp(b, a);
    munit_assert_int(cmp2, >, 0);

    td_release(a);
    td_release(b);
    return MUNIT_OK;
}

/* ---- str_vec_new ------------------------------------------------------- */

static MunitResult test_str_vec_new(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 10);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));
    munit_assert_int(v->type, ==, TD_STR);
    munit_assert_int(td_len(v), ==, 0);
    /* Pool starts as NULL (no long strings yet) */
    munit_assert_null(v->str_pool);
    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_new_zero_cap(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 0);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));
    munit_assert_int(v->type, ==, TD_STR);
    td_release(v);
    return MUNIT_OK;
}

/* ---- str_vec_append inline ---------------------------------------------- */

static MunitResult test_str_vec_append_inline(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);

    /* Append a short string (fits in 12 bytes) */
    v = td_str_vec_append(v, "hello", 5);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));
    munit_assert_int(td_len(v), ==, 1);

    /* Verify element is inline */
    td_str_t* elems = (td_str_t*)td_data(v);
    munit_assert_uint(elems[0].len, ==, 5);
    munit_assert_memory_equal(5, elems[0].data, "hello");

    /* Pool should still be NULL (no long strings) */
    munit_assert_null(v->str_pool);

    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_append_inline_12(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);

    /* Exactly 12 bytes — should still inline */
    v = td_str_vec_append(v, "123456789012", 12);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));

    td_str_t* elems = (td_str_t*)td_data(v);
    munit_assert_uint(elems[0].len, ==, 12);
    munit_assert_memory_equal(12, elems[0].data, "123456789012");
    munit_assert_null(v->str_pool);

    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_append_empty(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);

    v = td_str_vec_append(v, "", 0);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));

    td_str_t* elems = (td_str_t*)td_data(v);
    munit_assert_uint(elems[0].len, ==, 0);
    munit_assert_null(v->str_pool);

    td_release(v);
    return MUNIT_OK;
}

/* ---- str_vec_append pooled ---------------------------------------------- */

static MunitResult test_str_vec_append_pooled(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);

    /* 13 bytes — must go to pool */
    const char* long_str = "hello world!!";
    v = td_str_vec_append(v, long_str, 13);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));
    munit_assert_int(td_len(v), ==, 1);

    /* Pool should be allocated */
    munit_assert_ptr_not_null(v->str_pool);

    /* Element should have prefix and pool offset */
    td_str_t* elems = (td_str_t*)td_data(v);
    munit_assert_uint(elems[0].len, ==, 13);
    munit_assert_memory_equal(4, elems[0].prefix, "hell");
    munit_assert_uint(elems[0].pool_off, ==, 0);

    /* Verify pool contains the string */
    const char* pool_data = (const char*)td_data(v->str_pool);
    munit_assert_memory_equal(13, pool_data + 0, long_str);

    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_append_mixed(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);

    /* Mix inline and pooled */
    v = td_str_vec_append(v, "short", 5);
    v = td_str_vec_append(v, "this is a long string!", 22);
    v = td_str_vec_append(v, "tiny", 4);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));
    munit_assert_int(td_len(v), ==, 3);

    td_str_t* elems = (td_str_t*)td_data(v);
    /* First: inline */
    munit_assert_uint(elems[0].len, ==, 5);
    munit_assert_memory_equal(5, elems[0].data, "short");
    /* Second: pooled */
    munit_assert_uint(elems[1].len, ==, 22);
    munit_assert_memory_equal(4, elems[1].prefix, "this");
    /* Third: inline */
    munit_assert_uint(elems[2].len, ==, 4);
    munit_assert_memory_equal(4, elems[2].data, "tiny");

    td_release(v);
    return MUNIT_OK;
}

/* ---- str_vec_get ------------------------------------------------------- */

static MunitResult test_str_vec_get_inline(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);
    v = td_str_vec_append(v, "hello", 5);

    size_t len;
    const char* ptr = td_str_vec_get(v, 0, &len);
    munit_assert_ptr_not_null(ptr);
    munit_assert_size(len, ==, 5);
    munit_assert_memory_equal(5, ptr, "hello");

    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_get_pooled(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);
    v = td_str_vec_append(v, "this is a long string!", 22);

    size_t len;
    const char* ptr = td_str_vec_get(v, 0, &len);
    munit_assert_ptr_not_null(ptr);
    munit_assert_size(len, ==, 22);
    munit_assert_memory_equal(22, ptr, "this is a long string!");

    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_get_oob(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);
    v = td_str_vec_append(v, "hello", 5);

    size_t len;
    const char* ptr = td_str_vec_get(v, 1, &len);
    munit_assert_null(ptr);

    td_release(v);
    return MUNIT_OK;
}

/* ---- str_vec_set ------------------------------------------------------- */

static MunitResult test_str_vec_set_inline_to_inline(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);
    v = td_str_vec_append(v, "hello", 5);

    v = td_str_vec_set(v, 0, "world", 5);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));

    size_t len;
    const char* ptr = td_str_vec_get(v, 0, &len);
    munit_assert_size(len, ==, 5);
    munit_assert_memory_equal(5, ptr, "world");

    /* No pool needed */
    munit_assert_null(v->str_pool);

    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_set_pooled_to_pooled(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);
    v = td_str_vec_append(v, "original long string!", 21);

    v = td_str_vec_set(v, 0, "replacement string!!", 20);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));

    size_t len;
    const char* ptr = td_str_vec_get(v, 0, &len);
    munit_assert_size(len, ==, 20);
    munit_assert_memory_equal(20, ptr, "replacement string!!");

    /* Old 21 bytes are dead in pool */
    /* Pool used should be 21 (old dead) + 20 (new) = 41 */
    munit_assert_int(v->str_pool->len, ==, 41);

    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_set_oob(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);
    v = td_str_vec_append(v, "hello", 5);

    td_t* r = td_str_vec_set(v, 5, "x", 1);
    munit_assert_true(TD_IS_ERR(r));

    td_release(v);
    return MUNIT_OK;
}

/* ---- str_vec_compact --------------------------------------------------- */

static MunitResult test_str_vec_compact(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);

    /* Fill with pooled strings */
    v = td_str_vec_append(v, "original string one!", 20);
    v = td_str_vec_append(v, "original string two!", 20);

    /* Overwrite first — creates 20 dead bytes */
    v = td_str_vec_set(v, 0, "replacement str one!", 20);

    munit_assert_int(v->str_pool->len, ==, 60);  /* 20+20+20 */

    /* Compact */
    v = td_str_vec_compact(v);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));

    /* Pool should shrink: only 40 live bytes (string two + replacement) */
    munit_assert_int(v->str_pool->len, ==, 40);

    /* Strings still readable */
    size_t len;
    const char* p0 = td_str_vec_get(v, 0, &len);
    munit_assert_size(len, ==, 20);
    munit_assert_memory_equal(20, p0, "replacement str one!");

    const char* p1 = td_str_vec_get(v, 1, &len);
    munit_assert_size(len, ==, 20);
    munit_assert_memory_equal(20, p1, "original string two!");

    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_compact_noop(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);

    /* No pooled strings — compact should be a no-op */
    v = td_str_vec_append(v, "short", 5);
    v = td_str_vec_compact(v);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));
    munit_assert_null(v->str_pool);

    size_t len;
    const char* ptr = td_str_vec_get(v, 0, &len);
    munit_assert_size(len, ==, 5);
    munit_assert_memory_equal(5, ptr, "short");

    td_release(v);
    return MUNIT_OK;
}

static MunitResult test_str_vec_compact_all_dead(const void* params, void* fixture) {
    (void)params; (void)fixture;
    td_t* v = td_vec_new(TD_STR, 4);

    /* Append pooled, then overwrite to inline — all pool bytes dead */
    v = td_str_vec_append(v, "this is a long string!", 22);
    v = td_str_vec_set(v, 0, "short", 5);

    v = td_str_vec_compact(v);
    munit_assert_ptr_not_null(v);
    munit_assert_false(TD_IS_ERR(v));

    /* Pool should be freed entirely */
    munit_assert_null(v->str_pool);

    size_t len;
    const char* ptr = td_str_vec_get(v, 0, &len);
    munit_assert_size(len, ==, 5);
    munit_assert_memory_equal(5, ptr, "short");

    td_release(v);
    return MUNIT_OK;
}

/* ---- Suite definition -------------------------------------------------- */

static MunitTest str_tests[] = {
    { "/ptr_sso",       test_str_ptr_sso,       str_setup, str_teardown, 0, NULL },
    { "/ptr_long",      test_str_ptr_long,       str_setup, str_teardown, 0, NULL },
    { "/len",           test_str_len,             str_setup, str_teardown, 0, NULL },
    { "/cmp_equal",     test_str_cmp_equal,       str_setup, str_teardown, 0, NULL },
    { "/cmp_different", test_str_cmp_different,   str_setup, str_teardown, 0, NULL },
    { "/cmp_prefix",    test_str_cmp_prefix,      str_setup, str_teardown, 0, NULL },
    { "/vec_new",       test_str_vec_new,         str_setup, str_teardown, 0, NULL },
    { "/vec_new_zero",  test_str_vec_new_zero_cap, str_setup, str_teardown, 0, NULL },
    { "/vec_append_inline",    test_str_vec_append_inline,    str_setup, str_teardown, 0, NULL },
    { "/vec_append_inline_12", test_str_vec_append_inline_12, str_setup, str_teardown, 0, NULL },
    { "/vec_append_empty",     test_str_vec_append_empty,     str_setup, str_teardown, 0, NULL },
    { "/vec_append_pooled",    test_str_vec_append_pooled,    str_setup, str_teardown, 0, NULL },
    { "/vec_append_mixed",     test_str_vec_append_mixed,     str_setup, str_teardown, 0, NULL },
    { "/vec_get_inline",       test_str_vec_get_inline,       str_setup, str_teardown, 0, NULL },
    { "/vec_get_pooled",       test_str_vec_get_pooled,       str_setup, str_teardown, 0, NULL },
    { "/vec_get_oob",          test_str_vec_get_oob,          str_setup, str_teardown, 0, NULL },
    { "/vec_set_i2i",          test_str_vec_set_inline_to_inline, str_setup, str_teardown, 0, NULL },
    { "/vec_set_p2p",          test_str_vec_set_pooled_to_pooled, str_setup, str_teardown, 0, NULL },
    { "/vec_set_oob",          test_str_vec_set_oob,          str_setup, str_teardown, 0, NULL },
    { "/vec_compact",          test_str_vec_compact,          str_setup, str_teardown, 0, NULL },
    { "/vec_compact_noop",     test_str_vec_compact_noop,     str_setup, str_teardown, 0, NULL },
    { "/vec_compact_all_dead", test_str_vec_compact_all_dead, str_setup, str_teardown, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL },
};

MunitSuite test_str_suite = {
    "/str",
    str_tests,
    NULL,
    0,
    0,
};
