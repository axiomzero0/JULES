# JULES Frontend Language Surface

**Status:** Complete for the MVP milestone.
**Scope:** Every reserved keyword in the grammar is now either implemented or
rejected with a precise, actionable diagnostic. This document is the contract
between the surface syntax and the lowering.

The frontend maps the full user type system onto the **fixed scalar IR
lattice** (`core/son/types.h`): user TypeIds live at `kUserTyBase` (32+) and
are **sema-only** — the graph builder degrades them (backing integers,
`*i64`-width addresses) at every materialization site, so the 89-pass
optimizer and the backends remain entirely unaware of user types. Zero IR and
zero pass changes was a design constraint, honored except where noted.

---

## Declarations (source order = declaration-before-use)

| Form | Semantics | Lowering |
|---|---|---|
| `struct P { x: T, ... }` | value type; fields: scalars, pointers, enums/bitmasks/bitfields, nested structs | decomposed (below) |
| `enum E { A, B = 5, C }` | integer-backed (`i32` default, `: i32/i64/u32/u64`) | backing integer |
| `bitmask M { A, B = 4 }` | `u64`-backed flag set; auto `1<<n` | `u64` |
| `bitfield F(u64) { a: 3, b: 5 }` | packed bit ranges over `u64` | `u64` + shift/mask |
| `alias A = T;` | transparent name (folds for type identity) | the target |
| `trait T { fn m(&self) -> R; }` | method signatures | static dispatch only |
| `impl T { fn m(&self) {...} }` | inherent methods | direct calls |
| `impl Tr for T { ... }` | trait impl (checked against the trait) | direct calls |
| `extern fn f(x: f64) -> f64;` | C ABI via the system linker (`cc ... -lm`) | `CallSym` |
| `defer stmt;` / `defer { ... }` | runs at scope exit, LIFO | duplication at exits |

**Deliberately not implemented** (per engineering standard Rule 56, "no
premature invention"; both get precise diagnostics):

- `class` — the language is a static systems language with value structs and
  explicit `alloc()`; a parallel reference-type hierarchy has no spec and no
  use case in the suite.
- `dyn` — trait objects need vtable emission plus indirect calls in the
  IR/backend (fat pointers, `CallInd`); that is a backend milestone. Use
  static `impl Trait for Type` dispatch.

## Structs: two representations

- **Decomposed (the default):** a struct local becomes one memory-backed slot
  per LEAF field (`p.x`, `p.inner.y`), so SROA (pass 26) promotes each field to
  SSA — the fast path, SROA-by-construction.
- **Contiguous (forced by the sema escape analysis):** when the value's
  address is materialized — `&p`, a method receiver, a by-value argument —
  the local becomes ONE allocation of the struct size with offset addressing.
  The escape analysis runs after checking and flips the `Let`.

Pointers to structs (`*const P`, `*mut P`) are sema types that degrade to
`*i64` in the IR; field access through them is offset loads/stores.
`alloc(P)` / `alloc(P, n)` size from the struct table (recursion through
value fields is rejected — infinite size; use `*mut P`).

**ABI:** struct parameters pass as one pointer (address) + an entry copy in
the callee (value semantics preserved); struct returns use a hidden sret
pointer as the LAST parameter, ret becomes void. Both are uniform for caller
and callee. Extern fns are scalar-only (no C struct ABI).

## Field / method expressions

- `p.x` — field access; auto-deref one level on pointer bases (`pp.x`,
  `self.x`); pointer fields chain through the loaded pointer (`n.next.val`).
- `Color.Red` / `Perm.Read` — qualified enum/bitmask constants (folded at
  check time; locals shadow type names).
- `p.m(args)` — method call; inherent impls first, then trait impls
  (ambiguity between two traits is an error). Receiver: values materialize
  (escape-forced), pointers pass through.
- `m.has(Perm.X)` — bitmask sugar, lowers to `(m & X) != 0`.
- `&p` / `&p.f` — address of struct lvalues (forces the contiguous
  representation).

## Enum / bitmask / bitfield rules

- enums: `== !=` and `as` to/from the backing integer (unchecked both ways,
  documented); no arithmetic.
- bitmasks: `| & ^`, `~`, `== !=` between same-type values, `has()`,
  `as u64` / `u64 as M`; comparison against a literal `0` is allowed
  (the empty set).
- bitfields: named segments; reads lower to `(v >> shift) & mask`; writes to
  read-modify-write; literals fold to the packed `u64` constant.

## defer semantics

The body is DUPLICATED at every exit of its lexical scope: fall-through,
`break`, `continue`, `return`, and each loop-iteration end (Zig semantics).
`return e` evaluates `e` FIRST, then defers run (the returned value is the
pre-defer one). Defer bodies must not contain `break`/`continue`/`return`
(they would have no single meaning — rejected at check time).

## Fixed latent frontend bugs found by this round

1. `SymbolTable::intern` stored the caller's `string_view` in the map —
   mangled method names are temporaries, the dangling views corrupted the
   sorted map (nondeterministic lookup failures). Now stores the arena copy.
2. `*const T` never parsed: `const` is a keyword and the parser matched it
   with an identifier check. (Every existing test used `*mut`.)
3. Assigning to a parameter crashed the builder (null slot deref); params
   now materialize a slot on first assignment.
4. Pass 29 (heap-to-stack) promoted aggregate allocations (`alloc(T, n)`,
   struct buffers) into fixed 8-byte frame slots — heap corruption. It now
   promotes only scalar-sized allocations; aggregates keep their malloc.
