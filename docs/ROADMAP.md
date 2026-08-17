# Roadmap

## 0.1 — Foundation (current)

- dynamic scalar and collection values
- source locations and diagnostics
- expression execution and persistent scope
- strict JSON parser and serializer
- public C++23 host API, CLI, and tests

## 0.2 — Language core

- functions, lexical closures, explicit `self`, return, and exceptions
- property assignment and safe navigation
- classes, single inheritance, modules, `include`, and `prepend`
- blocks, `yield`, iterators, ranges, and comprehensions
- frozen grammar and conformance tests

## 0.3 — Bytecode runtime

- register bytecode format and verifier
- portable interpreter and call frames
- precise root tracking and initial stop-the-world collector
- module loader and deterministic standard-library boundary

## 0.4 — WebAssembly host

- WebAssembly build and small browser host ABI
- DOM, event, timer, fetch, and Crypto bindings
- Fiber scheduler integrated with the browser event loop
- opt-in ALK script loader for existing browsers

## 0.5 — JIT

- profiling and inline caches
- Tier-1 emitters for one architecture at a time
- W^X, unwind, deoptimization, and security validation
- SSA HIR and a measured Tier-2 optimization set

## Native browser integration

Native `application/alk` execution is a separate browser-engine project. It begins only after the bytecode verifier, host ABI, GC barriers, and security model are stable.
