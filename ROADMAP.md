# Roadmap

## Status legend

- **Complete**: implemented and covered by automated conformance tests.
- **Next**: the next planned release line.
- **Future**: designed direction whose implementation has not started.

## 0.1 — Foundation — Complete

- dynamic scalar and collection values
- source locations and diagnostics
- expression execution and persistent scope
- strict JSON parser and serializer
- public C++23 host API, CLI, examples, and dependency-free tests

## 0.2 — Language core — Complete

- functions, lexical closures, explicit `self`, `return`, `raise`, and `rescue`
- `if` / `unless`, postfix conditions, `else`, and `while`
- property and index assignment plus nil-safe `?.` navigation
- classes, single inheritance, `super`, modules, `include`, and `prepend`
- first-class blocks, `yield`, `each`, `map`, `select`, and `reduce`
- finite and endless ranges, finite `take`, and filtered list comprehensions
- frozen 0.2 grammar behavior covered by the `alk.v03` conformance suite

## 0.3 — Bytecode runtime and baseline JIT — Complete

- three-operand register bytecode format and structural verifier
- VM call frames for every user-defined function
- fully lowered scalar bytecode plus an explicit verified AST fallback opcode for complex constructs
- portable bytecode interpreter used whenever native specialization is unavailable or ineligible
- precise root tracing and a stop-the-world mark-sweep managed object heap
- stable object references that permit cyclic object graphs to be reclaimed
- deterministic module boundary with initial `std.json` and `std.math` modules
- x86-64 baseline JIT for eligible integer bytecode on Windows, Linux, and macOS
- runtime integer guards, checked-overflow deoptimization, and bytecode fallback
- W^X executable-memory transitions and instruction-cache synchronization
- observable bytecode, JIT, fallback, native-call, and collection counters through `ALK.jit_stats()`

The baseline native tier currently specializes straight-line `Int64` arithmetic (`+`, `-`, `*`, and unary negation). Strings, floating-point operations, calls, branches, objects, and complex language constructs remain in verified bytecode or its verified AST fallback opcode. This is a functioning JIT, not yet the optimizing tier described below.

## 0.4 — WebAssembly host — Next

- WebAssembly build and a small, versioned browser host ABI
- DOM, event, timer, Fetch, and Crypto bindings
- Fiber scheduler integrated with the browser event loop
- opt-in ALK script loader for existing browsers
- browser conformance and host-boundary security tests

## 0.5 — Optimizing JIT — Future

- hotness profiling and polymorphic inline caches
- bytecode-to-SSA HIR reconstruction
- guarded inlining, constant folding, dead-code elimination, and escape analysis
- deoptimization metadata for reconstructing interpreter frames
- AArch64 native emitter
- unwind metadata and platform control-flow hardening
- measured optimization gates for startup time, code size, and peak memory

## Native browser integration — Future

Native `<script type="application/alk">` execution is a separate browser-engine project. It begins only after the WebAssembly host ABI, bytecode verifier, GC barriers, native-code security model, and browser conformance suite are stable.
