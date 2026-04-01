# Datalog Engine Plan for Teide C

## Goal

Build a native Datalog evaluation engine inside Teide's C codebase. This is NOT a Lisp language layer — it's the Datalog compiler that translates rules into `td_graph_t` operation DAGs and evaluates them to fixpoint using Teide's existing vectorized execution engine.

## Context

Teide is a C17 columnar engine with:
- Lazy operation DAGs: `td_graph_new()` → `td_scan/filter/join/union_all/distinct/select` → `td_execute()`
- Optimizer: `td_optimize()` with SIP, predicate pushdown, fusion, DCE
- Morsel-driven parallel execution (1024 elements/morsel)
- CSR indices, Leapfrog Triejoin (`td_wco_join`), hash joins
- Already has: `td_union_all`, `td_distinct`, `td_join`, `td_wco_join`, `td_filter`, `td_select`, `td_eq`

The existing Rust-based Datalog evaluator (teide-cogno) uses `HashSet<Vec<Value>>` for relations and does row-at-a-time nested-loop evaluation. We want to replace this with native C Teide operations.

## What to Build

### Phase 1: C API additions (~200 lines)

**1a. `td_antijoin`** — the critical missing operation
- Hash anti-join: build hash table from right side, probe left side, emit rows with NO match
- Needed for: delta computation (set difference), stratified negation
- Add opcode `OP_ANTIJOIN` in `include/teide/td.h`
- Implement in `src/ops/exec.c` (follow `OP_JOIN` pattern but emit non-matches)
- Add `td_antijoin()` graph builder in `src/ops/graph.c`
- Add optimizer support in `src/ops/opt.c`

**1b. `td_table_count` / `td_table_empty`** — convergence helpers
- Simple: read the row count from a `td_t` table
- Needed for: fixpoint convergence check

**1c. `td_table_insert_row`** — single-row insertion for fact loading
- Insert one row into a columnar table (append to each column)
- Needed for: loading EDB facts before evaluation

### Phase 2: Datalog runtime (~800 lines)

**New files: `src/datalog/datalog.h`, `src/datalog/datalog.c`**

Data structures:
```c
// A Datalog relation backed by a Teide columnar table
typedef struct {
    td_t*  table;        // columnar table with named columns
    int    arity;         // number of columns
    char*  name;          // relation name (interned symbol)
    int    is_idb;        // 1 if this is a derived (intensional) relation
} dl_rel_t;

// A Datalog rule: head :- body
typedef struct {
    char*  head_pred;     // head predicate name
    int    head_arity;
    int*   head_vars;     // variable indices in head (-1 for constants)
    td_t** head_consts;   // constant values for non-variable positions
    
    int    n_body;        // number of body literals
    struct {
        int     type;     // DL_POS, DL_NEG, DL_CMP, DL_ASSIGN
        char*   pred;     // predicate name (for DL_POS/DL_NEG)
        int     arity;
        int*    vars;     // variable indices
        td_t**  consts;   // constants
        int     cmp_op;   // for DL_CMP: TD_EQ, TD_LT, etc.
    }* body;
} dl_rule_t;

// Full Datalog program
typedef struct {
    dl_rel_t*  rels;      // all relations (EDB + IDB)
    int        n_rels;
    dl_rule_t* rules;     // all rules
    int        n_rules;
    int**      strata;    // stratification: array of predicate-name arrays
    int*       n_strata;
} dl_program_t;
```

Functions:
```c
// Create/destroy program
dl_program_t* dl_program_new(void);
void          dl_program_free(dl_program_t* prog);

// Add EDB facts (loaded from existing Teide tables)
void dl_add_edb(dl_program_t* prog, const char* name, td_t* table, int arity);

// Add a rule
void dl_add_rule(dl_program_t* prog, dl_rule_t* rule);

// Compute stratification
int  dl_stratify(dl_program_t* prog);

// Evaluate to fixpoint using semi-naive evaluation
int  dl_eval(dl_program_t* prog);

// Query result
td_t* dl_query(dl_program_t* prog, const char* pred_name);
```

### Phase 3: Rule compiler (~600 lines)

The core innovation: compile each rule body into a `td_graph_t` DAG.

```c
// Compile one rule into a td_graph_t for one fixpoint iteration
// delta_rel: which body position uses the delta relation (semi-naive)
td_t* dl_compile_rule(
    dl_program_t* prog,
    dl_rule_t*    rule,
    int           delta_pos,     // -1 for initial pass, 0..n for semi-naive
    td_graph_t*   g
);
```

Compilation logic:
1. For each positive body atom: `td_scan(g, relation_table)` → `td_filter` on constants
2. For shared variables across atoms: `td_join` on matching columns
3. For negated atoms: `td_antijoin`
4. For comparisons: `td_filter` with comparison predicate
5. Project to head variables: `td_select`
6. The result is a td_graph_t node that produces new head tuples

### Phase 4: Semi-naive fixpoint loop (~200 lines)

```c
int dl_eval(dl_program_t* prog) {
    dl_stratify(prog);
    
    for (int s = 0; s < prog->n_strata; s++) {
        // Collect rules in this stratum
        dl_rule_t** stratum_rules = ...;
        
        // Phase A: Initial evaluation
        td_graph_t* g = td_graph_new(NULL);
        td_op_t* outputs[n_rules]; // one output per rule
        
        for each rule in stratum:
            outputs[i] = dl_compile_rule(prog, rule, -1, g);
        
        // Union all rule outputs
        td_op_t* all = outputs[0];
        for (i = 1..n):
            all = td_union_all(g, all, outputs[i]);
        
        // Deduplicate
        all = td_distinct(g, all, head_cols, n_head_cols);
        
        // Execute
        td_optimize(g, all);
        td_t* delta = td_execute(g, all);
        
        // Merge delta into IDB relation
        merge_delta(prog, head_pred, delta);
        
        // Phase B: Semi-naive loop
        while (td_table_count(delta) > 0) {
            g = td_graph_new(NULL);
            
            for each rule:
                for each positive body atom position:
                    outputs[k] = dl_compile_rule(prog, rule, pos, g);
            
            // union + distinct + antijoin against full relation
            all = union_all_outputs(outputs);
            all = td_distinct(g, all, ...);
            all = td_antijoin(g, all, td_scan(g, full_relation), ...);
            
            td_optimize(g, all);
            td_t* new_delta = td_execute(g, all);
            merge_delta(prog, head_pred, new_delta);
            delta = new_delta;
        }
    }
    return 0;
}
```

### Phase 5: Tests (~400 lines)

**New file: `test/test_datalog.c`**

Test cases:
1. **Simple fact query**: assert facts, query them back
2. **One-rule derivation**: grandparent(X,Z) :- parent(X,Y), parent(Y,Z)
3. **Transitive closure**: path(X,Y) :- edge(X,Y); path(X,Z) :- edge(X,Y), path(Y,Z)
4. **Stratified negation**: unreachable(X) :- node(X), not reachable(start,X)
5. **Multi-stratum**: mutual exclusion rules
6. **OR semantics**: two rules with same head, union of results
7. **Delta convergence**: verify fixpoint terminates correctly
8. **Performance**: transitive closure on 100K-edge graph, measure vs naive

## File Changes Summary

| File | Change | Lines |
|------|--------|-------|
| `include/teide/td.h` | Add `OP_ANTIJOIN`, `td_antijoin()`, `td_table_count()`, `td_table_insert_row()` | ~30 |
| `src/ops/exec.c` | Implement `OP_ANTIJOIN` executor | ~80 |
| `src/ops/graph.c` | Add `td_antijoin()` graph builder | ~30 |
| `src/ops/opt.c` | Optimizer support for antijoin | ~20 |
| `src/datalog/datalog.h` | New: Datalog data structures + API | ~80 |
| `src/datalog/datalog.c` | New: Rule compiler + fixpoint evaluator | ~1200 |
| `test/test_datalog.c` | New: Test suite | ~400 |
| `CMakeLists.txt` | Add datalog source files | ~5 |

**Total: ~1850 lines of new/modified C code**

## Build Order

1. Add `td_antijoin` to existing ops (test independently)
2. Add `td_table_count` and `td_table_insert_row` (trivial)
3. Create `src/datalog/` with data structures
4. Implement rule compiler (compile single rule → td_graph_t)
5. Implement fixpoint loop
6. Write tests, verify against reference
7. Wire into Cogno MCP as evaluation backend (later)

## Key Constraints

- **No Rust** — this is pure C, inside the Teide C codebase
- **No new dependencies** — use only Teide's existing allocator, hash tables, symbol tables
- **Follow Teide conventions**: `td_` prefix, `td_alloc`/`td_free` for memory, morsel-driven execution
- **Error returns**: `td_err_t` for functions, `TD_ERR_PTR()` for pointer-returning functions
- **Test with ASan + UBSan**: all tests must pass with sanitizers

## Implementation Status (2026-03-28)

All phases complete. All tests passing (debug + release).

| Phase | Status | Notes |
|-------|--------|-------|
| Phase 1: C API additions | **Done** | `OP_ANTIJOIN`, `td_antijoin()`, `td_table_insert_row()`, `td_table_count/empty` |
| Phase 2: Datalog runtime | **Done** | `dl_program_t`, `dl_rel_t`, `dl_rule_t` data structures |
| Phase 3: Rule compiler | **Done** | `dl_compile_rule()` with body-unique column naming |
| Phase 4: Semi-naive fixpoint | **Done** | `dl_eval()` with stratified negation, delta computation |
| Phase 5: Tests | **Done** | 11 Datalog tests + 5 antijoin/insert_row tests (all passing) |

### Test coverage
- Antijoin: basic, empty right, full match, multikey
- Table insert_row: append + verify
- Datalog: fact query, grandparent derivation, transitive closure (linear + cyclic), OR semantics, stratified negation, convergence, stratification
