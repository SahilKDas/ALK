# Runtime architecture

## Current 0.3 pipeline

```text
ALK source
  -> lexer with source locations
  -> parser and 0.2 syntax tree
  -> function bytecode compiler
  -> structural bytecode verifier
  -> register VM call frame
       |-> portable dynamic bytecode execution
       |-> verified AST fallback opcode for complex constructs
       `-> guarded x86-64 native specialization for eligible Int64 chunks
  -> precise managed heap and public alk::Value result
```

Top-level declarations and statements are evaluated from the parsed program. Every user-defined function owns a verified bytecode chunk and enters a VM call frame. Scalar expressions are lowered to normal register instructions. Functions containing constructs that are not individually lowered yet receive an explicit `execute_ast` instruction in their verified chunk rather than bypassing the VM.

## Register bytecode

Instructions use explicit destination and source registers. The current instruction set includes:

- constants, parameters, lexical loads, lexical stores, and moves;
- arithmetic, unary operations, equality, and ordered comparisons;
- unconditional and conditional jumps;
- verified AST-fragment execution;
- return.

Before execution or native compilation, the verifier checks register indices, constant and name indices, parameter indices, jump destinations, AST-fragment indices, instruction count, register count, and the presence of a return path. Bytecode is treated as untrusted structural input even though the current compiler produces it in memory.

## Call frames and lexical scope

A function call creates:

- a lexical scope linked to the function's captured closure;
- explicit parameter bindings, including explicit `self` for methods;
- an optional captured block binding for `yield`;
- a register file sized by the verified chunk;
- an active root entry for managed-heap tracing.

Nested functions retain their defining scope. Assignments are local to the current function scope; reads walk outward through captured scopes. Method lookup searches prepended modules, the class, included modules, then the parent class. `super` binds the receiver to the corresponding parent implementation.

## Baseline JIT

The baseline JIT consumes verified bytecode at runtime. It currently accepts straight-line chunks containing `Int64` parameters/constants, moves, addition, subtraction, multiplication, unary negation, and return.

On the first eligible call it emits x86-64 machine code into writable, non-executable memory. After emission the page is changed to read/execute, never read/write/execute, and the instruction cache is synchronized. The emitter supports the Windows x64 and System V x86-64 calling conventions.

Native entry points accept a checked integer argument vector and an overflow-status address. Dynamic type mismatches skip native execution. Arithmetic overflow is reported by generated code and deoptimizes to the checked bytecode path, preserving language semantics. Unsupported chunks remain portable and correct through the VM.

Runtime counters are exposed through `ALK.jit_stats()` so tests can distinguish bytecode compilation, native compilation, native calls, guards/fallbacks, and collection activity.

## Managed heap

Class instances live in a project-owned heap and are referenced by stable object IDs rather than owning pointers. A precise stop-the-world mark-sweep collector traces:

- global and active lexical scopes;
- arrays and maps;
- object fields;
- closure and block scopes;
- class and module method closures;
- explicit temporary value roots.

Because object-to-object fields contain non-owning IDs, unreachable cyclic object graphs can be reclaimed. Scalars, arrays, maps, functions, classes, and modules still use normal C++ value or shared ownership where cycles are not part of the managed object graph.

## Deterministic modules

`import module.path { Name }` resolves only against a runtime-owned module registry. It never searches the current directory or executes initialization files implicitly. The initial standard modules are:

- `std.json`, exporting `parse_json`;
- `std.math`, exporting `abs`.

Browser modules will be introduced through a versioned host ABI rather than direct filesystem discovery.

## Lightweight constraints

- no mandatory third-party runtime or test libraries;
- the portable VM works when native compilation is unavailable;
- JIT support is isolated behind a narrow native-code interface;
- complex features remain correct before being optimized;
- binary size, startup time, peak memory, allocation rate, and native-code size are release gates for future tiers;
- correctness, W^X discipline, verification, and deoptimization precede benchmark performance.

## Next architecture steps

0.4 adds the WebAssembly/browser host and Fiber scheduler. 0.5 reconstructs SSA HIR from bytecode for an optimizing tier, adds inline caches and guarded inlining, and introduces AArch64 emission. Neither is represented as implemented in the current runtime.
