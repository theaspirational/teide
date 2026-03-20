/*
 *   Copyright (c) 2024-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
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

static MunitResult test_arena_release_noop(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();

    /* Allocate a block and mark it as arena-owned */
    td_t* v = td_alloc(0);
    munit_assert_ptr_not_null(v);
    v->attrs |= TD_ATTR_ARENA;

    /* td_release should be a no-op — block should not be freed */
    td_release(v);

    /* If td_release freed it, accessing v->attrs would be UB / ASan error.
     * Since it's a no-op, we can still read it. */
    munit_assert_true(v->attrs & TD_ATTR_ARENA);

    /* Clean up: remove flag and release properly */
    v->attrs &= (uint8_t)~TD_ATTR_ARENA;
    td_release(v);

    td_heap_destroy();
    return MUNIT_OK;
}

static MunitTest arena_tests[] = {
    { "/release_noop", test_arena_release_noop, NULL, NULL, 0, NULL },
    { NULL, NULL, NULL, NULL, 0, NULL }
};

MunitSuite test_arena_suite = {
    "/arena", arena_tests, NULL, 1, 0
};
