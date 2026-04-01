# Phase 1: C API Additions — Detailed Implementation Plan

This plan covers Phase 1 of PLAN.md: `td_antijoin`, `td_table_count`, and `td_table_insert_row`.
These are prerequisites for the Datalog runtime (Phase 2+).

---

## Implementation Order

1. **Step 1: `OP_ANTIJOIN` opcode** — `include/teide/td.h`
2. **Step 2: `td_antijoin()` graph builder** — `src/ops/graph.c`
3. **Step 3: `OP_ANTIJOIN` executor** — `src/ops/exec.c`
4. **Step 4: Optimizer support** — `src/ops/opt.c` + `src/ops/fuse.c`
5. **Step 5: Plan printer** — `src/ops/dump.c`
6. **Step 6: Graph pointer fixups** — `src/ops/graph.c` (fixup_ext_ptrs)
7. **Step 7: `td_table_count` / `td_table_empty`** — `include/teide/td.h` + `src/table/table.c`
8. **Step 8: `td_table_insert_row`** — `include/teide/td.h` + `src/table/table.c`
9. **Step 9: Tests** — `test/test_exec.c` + `test/test_main.c`

---

## Step 1: Add `OP_ANTIJOIN` Opcode

**File:** `include/teide/td.h`

### 1a. Add opcode constant

**Location:** After line 406 (`#define OP_UNION_ALL 84`), insert:

```c
#define OP_ANTIJOIN      85   /* anti-join: left rows with NO match in right */
```

### 1b. Add `td_antijoin()` declaration

**Location:** After `td_union_all` declaration (~line 898), insert:

```c
/* Anti-join: emit left-side rows that have NO match on the right side.
 * Semantics: for each left row, probe right hash table on key columns;
 * if NO match found, emit the left row. Only left-side columns appear
 * in the output (right-side columns are not included).
 * Used for: set difference (delta computation), stratified negation. */
td_op_t* td_antijoin(td_graph_t* g,
                      td_op_t* left_table, td_op_t** left_keys,
                      td_op_t* right_table, td_op_t** right_keys,
                      uint8_t n_keys);
```

**Signature rationale:**
- Mirrors `td_join()` at line 853–856 but without `join_type` parameter (antijoin is always "anti").
- Uses `ext->join` union member (same as `OP_JOIN`). We reuse `join_type` field internally with a sentinel value (e.g., `3`) to distinguish antijoin from inner/left/full.
- Output is LEFT-side only (no right columns gathered) — key semantic difference from OP_JOIN.

### 1c. Add `td_table_count`, `td_table_empty`, `td_table_insert_row` declarations

**Location:** After `td_table_nrows` declaration (~line 763), insert:

```c
/* Convenience: row count as int64_t (same as td_table_nrows, but named for
 * Datalog convergence checks). Returns 0 for NULL/error tables. */
static inline int64_t td_table_count(td_t* tbl) { return td_table_nrows(tbl); }

/* Check if table has zero rows. */
static inline bool td_table_empty(td_t* tbl) { return td_table_nrows(tbl) == 0; }

/* Append one row to a flat columnar table. vals[] must have one element per
 * column, each pointing to a value of the correct type and size.
 * Returns updated table pointer (may COW/realloc). */
td_t* td_table_insert_row(td_t* tbl, const void** vals);
```

---

## Step 2: `td_antijoin()` Graph Builder

**File:** `src/ops/graph.c`

**Location:** After `td_union_all` implementation (~line 1165–1167), insert new function.

### Function implementation

```c
td_op_t* td_antijoin(td_graph_t* g,
                      td_op_t* left_table, td_op_t** left_keys,
                      td_op_t* right_table, td_op_t** right_keys,
                      uint8_t n_keys) {
    // Pattern: identical to td_join() at lines 709-749, with these differences:
    // 1. ext->base.opcode = OP_ANTIJOIN (not OP_JOIN)
    // 2. ext->join.join_type = 3  (sentinel: antijoin)
    // 3. No join_type parameter in signature
    //
    // Step-by-step (mirroring td_join):
    // a) Save IDs before alloc (pointers invalidated by realloc)
    // b) graph_alloc_ext_node_ex(g, keys_sz * 2) for trailing key arrays
    // c) Re-resolve pointers via g->nodes[saved_id]
    // d) Fill ext->base: opcode=OP_ANTIJOIN, arity=2, inputs, out_type=TD_TABLE
    // e) Fill ext->join: left_keys, right_keys, n_join_keys, join_type=3
    // f) Sync g->nodes[ext->base.id] = ext->base
}
```

**Key pattern from `td_join` (line 709–749):**
1. Save all node IDs as uint32_t before allocation (lines 713–719)
2. Allocate with `graph_alloc_ext_node_ex(g, keys_sz * 2)` (line 723)
3. Re-resolve all pointers via `&g->nodes[saved_id]` (lines 726–727)
4. Store key pointers in trailing memory via `EXT_TRAIL(ext)` (lines 737–743)
5. Sync back: `g->nodes[ext->base.id] = ext->base` (line 747)

**Gotcha:** Must save ALL node IDs as `uint32_t` before calling `graph_alloc_ext_node_ex` because the allocation may trigger `g->nodes` realloc, invalidating all `td_op_t*` pointers. This is the pattern used throughout graph.c (see lines 713–720).

---

## Step 3: `OP_ANTIJOIN` Executor

**File:** `src/ops/exec.c`

### 3a. Add `exec_antijoin` static function

**Location:** After `exec_join` cleanup (~line 9712), before `exec_window_join`.

This is the heart of the implementation. It uses a simplified version of the chained HT join path (lines 9367–9577) but **only emits non-matching left rows**.

```c
static td_t* exec_antijoin(td_graph_t* g, td_op_t* op,
                            td_t* left_table, td_t* right_table) {
    // 1. Error checks (mirror exec_join lines 9101-9103)
    // 2. Get ext node, extract n_keys, key vectors (lines 9105-9131)
    // 3. Build chained HT on right side (lines 9367-9430)
    //    - Same hash table build: ht_heads[], ht_next[]
    //    - Same join_build_fn dispatch
    // 4. Probe phase — INVERTED logic:
    //    - For each left row, probe HT
    //    - If NO match found → include in output (emit left row index)
    //    - If match found → skip
    //    - Output: single l_idx[] array (no r_idx needed)
    // 5. Gather: build result table with LEFT columns only
    //    - For each left column: allocate output, gather from l_idx
    //    - Do NOT include right columns (key semantic difference)
    // 6. Cleanup
}
```

**Why chained HT only (no radix path):**
- Antijoin in Datalog context operates on delta relations (small)
- Radix path complexity not justified for the typical use case
- Can add radix path later if needed (the threshold check at line 9151 can gate it)

**Alternative: radix path support**
If we want both paths, the radix path is actually simpler for antijoin than for join because:
- No pair counting needed (just a boolean "matched" per left row)
- Single-pass probe: build HT on right, scan left, mark unmatched
- Gather: collect indices where left row was unmatched

**Recommended approach:** Start with chained HT only. The structure allows adding the radix path later by checking `right_rows > TD_PARALLEL_THRESHOLD`.

### 3b. Add `case OP_ANTIJOIN` to `exec_node` switch

**Location:** After `case OP_JOIN` block (~line 12830–12847), insert:

```c
case OP_ANTIJOIN: {
    td_t* left = exec_node(g, op->inputs[0]);
    td_t* right = exec_node(g, op->inputs[1]);
    if (!left || TD_IS_ERR(left)) { if (right && !TD_IS_ERR(right)) td_release(right); return left; }
    if (!right || TD_IS_ERR(right)) { td_release(left); return right; }
    /* Compact lazy selection before antijoin */
    if (g->selection && left && !TD_IS_ERR(left) && left->type == TD_TABLE) {
        td_t* compacted = sel_compact(g, left, g->selection);
        td_release(left);
        td_release(g->selection);
        g->selection = NULL;
        left = compacted;
    }
    td_t* result = exec_antijoin(g, op, left, right);
    td_release(left);
    td_release(right);
    return result;
}
```

This mirrors exactly the `case OP_JOIN` pattern at lines 12830–12847.

### 3c. `exec_antijoin` detailed algorithm

```
Phase 1: Build hash table on RIGHT side
  - Identical to exec_join chained HT path (lines 9367–9430)
  - ht_heads[ht_cap], ht_next[right_rows]
  - Hash all right key columns, build chained HT

Phase 2: Probe LEFT side, collect non-matching indices
  - Allocate l_idx[left_rows] (worst case: all left rows pass)
  - For each left row r in [0, left_rows):
    - Hash left key columns
    - Walk HT chain: if ANY right row matches all keys → skip
    - If NO match → l_idx[out_count++] = r
  - This is simpler than join probe: no pair counting needed,
    single boolean decision per left row

Phase 3: Gather LEFT columns only
  - For each column c in left_table:
    - Allocate output column of size out_count
    - Gather: out[i] = src[l_idx[i]]
  - Build result table with gathered columns

Phase 4: Cleanup
  - Free HT buffers, index array
  - Return result table
```

**Reuse opportunity:** The hash computation and HT build can reuse `join_build_fn` and `join_build_ctx_t` directly. The probe is custom (inverted match logic).

### 3d. Key comparison logic

The key comparison in the probe loop must match `exec_join`'s key comparison. Looking at the join probe functions in exec.c, key comparison is done by:
1. Computing hash of left key columns
2. Walking HT chain at `ht_heads[hash & mask]`
3. For each candidate right row: comparing all key column values

For multi-key comparison, this needs to handle:
- `TD_I64`, `TD_SYM` (integer comparison via `read_col_i64`)
- `TD_F64` (double comparison)
- Key vector type dispatch

---

## Step 4: Optimizer Support

**File:** `src/ops/opt.c`

OP_ANTIJOIN reuses the `ext->join` union (same layout as OP_JOIN), so all optimizer passes that handle OP_JOIN need parallel OP_ANTIJOIN cases. There are **6 locations** where OP_JOIN appears in opt.c:

### 4a. Pass 1: Type inference traversal (line 126)

Add `case OP_ANTIJOIN:` alongside `case OP_JOIN:` in the type inference post-order traversal. Since OP_ANTIJOIN uses the same `ext->join` layout, the code block is identical:

```c
case OP_JOIN:
case OP_ANTIJOIN:   // <-- ADD THIS
    for (uint8_t k = 0; k < ext->join.n_join_keys; k++) {
        ...
    }
    break;
```

**Locations in opt.c (all 6):**

| Line | Pass | Action |
|------|------|--------|
| 126 | Type inference traversal | Add `case OP_ANTIJOIN:` |
| 564 | Constant folding traversal | Add `case OP_ANTIJOIN:` |
| 687 | DCE mark_live ext traversal guard | Add `n->opcode == OP_ANTIJOIN` to condition |
| 710 | DCE mark_live ext switch | Add `case OP_ANTIJOIN:` |
| 957 | Predicate pushdown pointer fixup | Add `case OP_ANTIJOIN:` |
| 1514 | Fusion pass live traversal | Add `case OP_ANTIJOIN:` |

**File:** `src/ops/fuse.c`

| Line | Pass | Action |
|------|------|--------|
| 113 | Fusion ref count ext guard | Add `n->opcode == OP_ANTIJOIN` to condition |
| 136 | Fusion ref count ext switch | Add `case OP_ANTIJOIN:` |

### 4b. Pattern for each location

Since OP_ANTIJOIN uses the exact same `ext->join` struct layout as OP_JOIN, the fix is mechanical: wherever there's `case OP_JOIN:`, add `case OP_ANTIJOIN:` as a fallthrough.

---

## Step 5: Plan Printer

**File:** `src/ops/dump.c`

### 5a. Opcode name (line 93)

After `case OP_JOIN: return "JOIN";`, add:

```c
case OP_ANTIJOIN:  return "ANTIJOIN";
```

### 5b. Extended info printing (line 162)

After the `case OP_JOIN:` block (lines 162–169), add:

```c
case OP_ANTIJOIN:
    if (ext)
        fprintf(f, "(keys=%u)", ext->join.n_join_keys);
    break;
```

### 5c. Recursive child printing

The dump function's recursive child printing section (line 200+) needs OP_ANTIJOIN to traverse `ext->join.left_keys` and `ext->join.right_keys` children, same as OP_JOIN. Find the `case OP_JOIN:` in the recursive section and add `case OP_ANTIJOIN:` as fallthrough.

---

## Step 6: Graph Pointer Fixups

**File:** `src/ops/graph.c`

### 6a. `graph_fixup_ext_ptrs` (line 39)

In the switch at line 47, `case OP_JOIN:` handles pointer fixup for the join ext node. Add `case OP_ANTIJOIN:` as fallthrough:

```c
case OP_JOIN:
case OP_ANTIJOIN:   // <-- ADD THIS
    for (uint8_t k = 0; k < ext->join.n_join_keys; k++)
        ext->join.left_keys[k] = graph_fix_ptr(ext->join.left_keys[k], delta);
    if (ext->join.right_keys) {
        for (uint8_t k = 0; k < ext->join.n_join_keys; k++)
            ext->join.right_keys[k] = graph_fix_ptr(ext->join.right_keys[k], delta);
    }
    break;
```

---

## Step 7: `td_table_count` / `td_table_empty`

These are trivial inline functions in the header. See Step 1c above.

`td_table_count` is just an alias for `td_table_nrows` (already exists at line 763). The Datalog runtime will call `td_table_count(delta)` for convergence checks.

`td_table_empty` is `td_table_nrows(tbl) == 0`.

Both defined as `static inline` in `td.h` — no .c file changes needed.

---

## Step 8: `td_table_insert_row`

**File:** `src/table/table.c`

**Location:** After `td_table_schema` (~line 262), add new function.

### Function signature

```c
td_t* td_table_insert_row(td_t* tbl, const void** vals) {
    if (!tbl || TD_IS_ERR(tbl)) return tbl;

    /* COW the table */
    tbl = td_cow(tbl);
    if (!tbl || TD_IS_ERR(tbl)) return tbl;

    int64_t ncols = tbl->len;
    td_t** cols = tbl_col_slots(tbl);

    for (int64_t c = 0; c < ncols; c++) {
        td_t* col = cols[c];
        if (!col || TD_IS_ERR(col)) continue;

        /* td_vec_append handles COW + grow internally.
         * vals[c] points to one element of the column's type. */
        td_t* new_col = td_vec_append(col, vals[c]);
        if (!new_col || TD_IS_ERR(new_col)) return new_col;

        if (new_col != col) {
            /* vec_append returned a new allocation (COW or grow) */
            td_release(col);
            td_retain(new_col);
            cols[c] = new_col;
        }
    }

    return tbl;
}
```

**Key patterns used:**
- `td_cow()` for exclusive ownership (line 90 in table.c)
- `tbl_col_slots()` for column access (line 44)
- `td_vec_append()` handles COW + capacity growth (vec.c:102)
- When `td_vec_append` returns a new pointer, release old, retain new, update slot

**Gotcha — `td_vec_append` ownership semantics:**
Looking at `td_vec_append` (vec.c:102), it calls `td_cow(vec)` internally and may return a different pointer via `td_scratch_realloc`. The caller gets a new owned reference. So:
- If `new_col != col`: the old `col` pointer is now stale (COW'd or realloc'd). But `td_vec_append` already freed the old block internally via `td_scratch_realloc`, so we must NOT call `td_release(col)` — it's already freed.
- Actually, looking more carefully at `td_cow`: it increments rc of the copy and decrements rc of the original. And `td_scratch_realloc` copies + frees. So after `td_vec_append`, if the return differs, the old pointer is invalid.

**Corrected logic:**
```c
td_t* new_col = td_vec_append(col, vals[c]);
if (new_col != col) {
    /* td_vec_append consumed the old ref (via cow/realloc).
     * The table slot still holds the old pointer. Update it.
     * The new_col is already owned (rc=1 from alloc). */
    cols[c] = new_col;
    /* No release of old col — td_vec_append already freed it.
     * No retain of new_col — it's already at rc=1. */
}
```

Wait — this needs more careful analysis. The table holds a `td_retain`'d reference to each column. Let's trace:

1. `td_table_add_col` does `td_retain(col_vec)` (table.c:119)
2. So `col` in the table has rc >= 2 (original + table's retain)
3. `td_vec_append(col, ...)` calls `td_cow(col)`:
   - If rc > 1: allocates copy, decrements old rc, returns copy with rc=1
   - If rc == 1: returns same pointer
4. If COW happened: old `col` still has rc >= 1 (table's retain is still there). The copy has rc=1.
5. We need to: release the table's old reference, store the new copy

**Actually**: `td_cow` decrements the original's rc. But it doesn't go through our table slot. The table slot still points to the original, which now has rc decremented by 1. So:

- Before: col has rc=N (table holds one ref, maybe others)
- After td_cow: if rc was > 1, original has rc=N-1, copy has rc=1
- The table slot still points to original (rc=N-1)
- We need to release the table's reference (the slot) and replace with copy

**Correct implementation:**
```c
td_t* new_col = td_vec_append(col, vals[c]);
if (!new_col || TD_IS_ERR(new_col)) return new_col;
if (new_col != col) {
    /* vec_append returned a new block (COW'd or grew).
     * Table still holds the old reference in cols[c]. */
    td_release(col);       /* drop table's old ref */
    td_retain(new_col);    /* table takes ownership of new ref */
    cols[c] = new_col;
}
```

Hmm, but there's another subtlety. `td_vec_append` calls `td_cow(vec)` first. If the vec was shared (rc>1), `td_cow` creates a copy and decrements original's rc. Then `td_vec_append` may call `td_scratch_realloc` on the copy. The returned pointer is the final result.

After this: the original `col` pointer still has rc = (original_rc - 1) because `td_cow` decremented it. The table slot still points to `col`. So the table's "ownership" reference is still valid (it was counted in original_rc). We need to release it and replace.

But wait — `td_cow` returns the copy and the original is now at rc-1. But `td_cow` doesn't free the original (rc went from N to N-1, still > 0). So the original is still alive. The table's reference to `col` is still valid.

So the correct sequence is:
1. Call `td_vec_append(col, vals[c])` — may return new pointer
2. If new: `td_release(cols[c])` to drop table's old reference
3. `cols[c] = new_col` — store new pointer
4. `td_retain(new_col)` is NOT needed because `td_vec_append` returns with rc=1 and we're transferring that ownership to the table slot

Actually, `td_vec_append` returns an owned reference. The table slot needs to own it too. But the table already owned the old one. So: release old, store new (taking ownership of the returned ref).

No `td_retain` needed because `td_vec_append` returns with rc=1 (fresh alloc). The table takes ownership of that rc=1 reference.

**Final correct logic:**
```c
td_t* new_col = td_vec_append(col, vals[c]);
if (!new_col || TD_IS_ERR(new_col)) return new_col;
if (new_col != col) {
    td_release(col);     /* drop table's old ref */
    cols[c] = new_col;   /* table adopts the new ref (rc=1 from append) */
}
```

**Gotcha alert:** If `td_table_insert_row` fails partway through (some columns appended, some not), the table is left in an inconsistent state (columns have different lengths). This is acceptable for the Datalog use case where we control the input, but should be documented. A transactional approach would require saving/restoring column pointers, which adds complexity.

---

## Step 9: Tests

**File:** `test/test_exec.c`

### 9a. `test_exec_antijoin` — basic antijoin

```c
static MunitResult test_exec_antijoin(const void* params, void* data) {
    (void)params; (void)data;
    td_heap_init();
    td_sym_init();

    /* Left: id(I64) = {1, 2, 3, 4} */
    /* Right: id(I64) = {2, 4} */
    /* Expected: antijoin on id → {1, 3} */

    // Build left and right tables (pattern from test_exec_join, line 691–727)
    // Call td_antijoin(g, left_op, lk_arr, right_op, rk_arr, 1)
    // Assert: result has 2 rows
    // Assert: result id column = {1, 3}
    // Assert: result has same columns as left (no right columns)

    // Cleanup...
}
```

### 9b. `test_exec_antijoin_empty_right` — right table empty → all left rows pass

### 9c. `test_exec_antijoin_full_match` — all left rows match → empty result

### 9d. `test_exec_antijoin_multikey` — multi-column key antijoin

### 9e. `test_table_insert_row` — insert rows and verify

**File:** `test/test_main.c`

Register new tests in the test suite array (line ~136).

---

## Potential Gotchas

### G1: ext->join reuse for OP_ANTIJOIN
OP_ANTIJOIN reuses the `ext->join` struct. This means `join_type` field is present but should be set to a sentinel (3) or ignored. All code that checks `ext->join.join_type == 0/1/2` must not accidentally match antijoin. Verify:
- `exec_join` checks join_type for inner(0)/left(1)/full(2) — antijoin won't reach exec_join.
- dump.c prints join_type — antijoin has its own case.

### G2: Pointer invalidation in graph builders
Every `graph_alloc_ext_node_ex` call can realloc `g->nodes`. ALL `td_op_t*` pointers must be saved as uint32_t IDs before the call and re-resolved after. This is the most common bug pattern in graph.c.

### G3: Ownership in td_table_insert_row
`td_vec_append` returns an owned reference. When it returns a different pointer than input, the table slot's old reference must be released. But the caller must NOT double-free the vec_append result. See Step 8 analysis above.

### G4: Antijoin output schema
OP_ANTIJOIN only outputs LEFT table columns. This is different from OP_JOIN which outputs left + non-key right columns. The gather phase must only iterate left columns.

### G5: Empty table fast paths
Like OP_UNION_ALL (exec.c:13288–13298), antijoin should handle empty tables efficiently:
- If right is empty → return left (all rows pass, no matches possible)
- If left is empty → return empty table

### G6: Selection compaction before antijoin
Must compact lazy selection before antijoin (same pattern as OP_JOIN at line 12836–12841). Antijoin needs dense data for hash table build.

### G7: GLOB_RECURSE in CMakeLists.txt
CMakeLists.txt uses `file(GLOB_RECURSE TEIDE_SOURCES CONFIGURE_DEPENDS "src/**/*.c")` (line 12). New files under `src/` are auto-discovered. **No CMakeLists.txt change needed** for table.c modifications. If we later add `src/datalog/datalog.c`, it will be auto-discovered too.

### G8: Thread-local heap and scratch allocation
`exec_antijoin` allocates scratch buffers via `scratch_alloc`/`scratch_calloc` which use `td_alloc` (buddy allocator). These are thread-local. Ensure all scratch buffers are freed via `scratch_free` before returning, including error paths. Use the `goto cleanup` pattern from `exec_join`.

### G9: Cancel check
Add `CHECK_CANCEL_GOTO(pool, antijoin_cleanup)` after HT build and after probe loop, matching exec_join pattern.

---

## Lines-of-Code Estimate

| File | Change | Lines |
|------|--------|-------|
| `include/teide/td.h` | OP_ANTIJOIN opcode + 3 function declarations | ~15 |
| `src/ops/graph.c` | `td_antijoin()` builder + fixup_ext_ptrs case | ~50 |
| `src/ops/exec.c` | `exec_antijoin()` + case OP_ANTIJOIN | ~150 |
| `src/ops/opt.c` | 6 × `case OP_ANTIJOIN:` additions | ~15 |
| `src/ops/fuse.c` | 2 × `case OP_ANTIJOIN:` additions | ~5 |
| `src/ops/dump.c` | Opcode name + ext info + recursive child | ~10 |
| `src/table/table.c` | `td_table_insert_row()` | ~30 |
| `test/test_exec.c` | 5 test functions | ~200 |
| `test/test_main.c` | Register tests | ~5 |

**Total: ~480 lines**

---

## Verification Checklist

After implementation, verify:

- [ ] `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build` — clean build
- [ ] `cd build && ctest --output-on-failure` — all existing tests pass
- [ ] New antijoin tests pass
- [ ] ASan + UBSan: no memory errors
- [ ] `td_table_insert_row` test: verify row count, column values
- [ ] `td_antijoin` with empty left/right tables
- [ ] `td_antijoin` with all-match / no-match cases
- [ ] `td_antijoin` with multi-column keys
- [ ] Plan printer: `td_graph_dump` shows ANTIJOIN node correctly
