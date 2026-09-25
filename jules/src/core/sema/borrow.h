// Strict borrow checker — OPT-IN via `import strict;` (never default).
//
// Design contract (docs/language_surface.md "Strict mode"):
//   * The borrow checker must not block language features: it is the
//     developer's choice. Without the import, nothing is checked and no
//     diagnostic fires — raw-pointer freedom is the default surface.
//   * With `import strict;`, Rust-shaped ownership/borrow rules are enforced
//     at compile time, flow-sensitively, per function, with interprocedural
//     return-flow summaries:
//       - every allocation has exactly ONE owner at a time,
//       - pointer values MOVE on rebinding (let / var / assignment / return /
//         struct-field store / non-const pointer casts),
//       - `as *const T` creates read-only shared views (copyable; while any
//         is live, the allocation cannot be written or freed),
//       - `&x` takes a UNIQUE borrow of a struct local (freezes it while
//         the borrowed pointer is live),
//       - call arguments are LENT for the call and returned to the caller
//         afterwards (the callee cannot free or move them),
//       - `free` consumes: double free, use after free, use after move,
//         free-of-borrowed, and writes through shared views are errors,
//       - leaks are SAFE (same as Rust's mem::forget): an unfreed owner at
//         scope exit is never an error.
//
// Heap-resident storage (array slots) cannot run drops in the MVP, so owned
// pointers may only move into HEAP STRUCT fields (trackable identities:
// free via the field itself); moving an owned pointer into a heap ARRAY
// slot is rejected — store a shared view instead. Loading a pointer out of
// a heap struct field yields a shared view; the owning free is
// `free(x.field)`. Local struct field loads MOVE (partial moves), matching
// Rust's Box-out-of-local-struct semantics.
#pragma once

#include "core/sema/sema.h"

namespace jules {

// Entry point: called by Sema::run() only when out.strict_mode is set and
// type checking already succeeded. Returns false on any violation
// (diagnostics already reported through `diag`).
bool run_borrow_check(ModuleAst& mod, SemaModule& out, Diagnostics& diag,
                      SymbolTable& syms);

} // namespace jules
