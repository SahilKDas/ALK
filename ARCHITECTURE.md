# Runtime architecture

## Current 0.1 foundation

```text
source -> lexer -> direct parser/evaluator -> alk::Value
                                      |-> strict JSON reader/writer
                                      |-> host-visible diagnostics
```

The current implementation is intentionally dependency-free and compact. It uses shared array/map storage to give collection values reference semantics while keeping scalar values cheap. Runtime scope persists across calls to `Runtime::execute_script`.

This stage is a semantic prototype. It does not claim bytecode, GC, JIT, Fibers, WebAssembly, or DOM support.

## Target pipeline

```text
ALK source
  -> C23-compatible lexer boundary
  -> parser and semantic validation
  -> three-operand register bytecode
  -> portable Tier-0 interpreter
  -> optional Tier-1 baseline JIT
  -> optional Tier-2 SSA optimizing JIT
  -> native browser host or WebAssembly host
```

### Tier 0

The portable baseline must use standard C++23 switch dispatch. Computed-goto dispatch may be an opt-in compiler-specific optimization; it cannot be the only implementation because computed goto is not part of C23 or C++23. Bytecode files will be versioned, little-endian, bounds-checked, and treated as untrusted input.

### Tier 1

The baseline JIT will translate hot bytecode blocks directly to x86-64 or AArch64 machine code. Platform-specific executable-memory allocation, instruction-cache synchronization, W^X transitions, unwind metadata, and control-flow protections must sit behind a narrow interface.

### Tier 2

The optimizing tier reconstructs typed SSA from bytecode and feedback. Initial passes are guarded inlining, constant folding, dead-code elimination, and escape analysis. Every speculative optimization must carry deoptimization metadata back to a valid interpreter frame.

### Memory management

The target collector is precise and generational:

- copying nursery for young objects;
- old-generation tracing with compaction where host constraints permit;
- exact stack maps for JIT frames;
- write barriers shared by interpreter and JIT;
- host handles for DOM and other externally owned objects.

Concurrent old-generation collection is a later optimization, not an MVP requirement. A stop-the-world collector should be made correct and measurable first.

### Browser integration

Two hosts are planned:

1. **WebAssembly fallback**: portable deployment in existing browsers, with browser APIs reached through a deliberately small host ABI.
2. **Native engine integration**: direct Blink/Gecko host bindings and `application/alk` script handling, requiring changes in a browser engine.

Direct DOM wrappers avoid ALK-level JSON serialization. A WebAssembly host may still cross a browser JavaScript ABI depending on platform support; documentation must not claim that bridge cost is absent in that configuration.

## Lightweight constraints

- no mandatory third-party libraries;
- one core library plus one CLI;
- features are compile-time selectable;
- interpreter remains usable when all JITs and browser bindings are disabled;
- binary size, startup time, peak memory, and allocation rate become release gates once bytecode exists;
- correctness and security precede peak benchmark performance.
