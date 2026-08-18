# ALK

ALK is a dynamically typed, JIT-compiled scripting language designed as a first-class web language. Its object model combines Ruby 2.6-style blocks and mixins with explicit `self`, predictable lexical scope, native JSON-compatible literals, and a planned direct browser API surface.

The current **0.3 development runtime** is a dependency-free C++23 implementation with a verified register VM, precise managed object heap, and guarded x86-64 baseline JIT. Browser hosting and the optimizing JIT are later milestones.

## Implemented

- dynamic scalars, strings, arrays, maps, objects, functions, blocks, modules, and ranges
- functions, lexical closures, explicit `self`, return, exceptions, conditionals, and loops
- classes, single inheritance, `super`, `include`, and `prepend`
- property/index assignment and nil-safe `?.` navigation
- `yield`, `each`, `map`, `select`, and `reduce`
- endless ranges and filtered list comprehensions
- strict JSON parsing and serialization
- deterministic `std.json` and `std.math` imports
- verified three-operand register bytecode and VM call frames
- precise stop-the-world mark-sweep collection for managed objects
- runtime x86-64 machine-code generation with W^X memory protection
- checked JIT guards, overflow deoptimization, and portable VM fallback
- public C++23 `alk::Runtime` / `alk::Value` host API

See [Language reference](LANGUAGE.md), [Runtime architecture](ARCHITECTURE.md), and [Roadmap](ROADMAP.md).

## Build and test

Requirements: CMake 3.25+ and a C++23 compiler. No package manager or third-party runtime/test library is used.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a file or inline source:

```sh
build/alk examples/v03.alk
build/alk -e 'puts {"answer": 42}["answer"]'
```

On Windows the executable is `build/alk.exe`. Multi-config generators may place it under `build/Release/`.

## JIT example

```alk
def mul_add(a, b, c)
  return a * b + c
end

puts mul_add(6, 7, 1)
puts ALK.jit_stats().to_json()
```

Eligible integer bytecode is compiled to native x86-64 code on first invocation. Other types and complex chunks remain correct through verified bytecode. The current baseline JIT is intentionally narrow; SSA optimization and AArch64 emission are future work.

## Browser status

ALK is not yet executable through `<script type="application/alk">` in existing browsers. The 0.4 milestone adds the WebAssembly host, browser API bindings, and Fiber/event-loop integration. Native Blink/Gecko integration follows only after that host boundary is stable.

## Project rules

ALK is an original implementation. Imported code is prohibited unless released under CC0 or the Unlicense, or the project owner gives explicit written permission for a recorded exception. Read [PROVENANCE.md](PROVENANCE.md) before contributing.

The ALK compiler and runtime source code contained in this repository is licensed under [GPL-3.0](LICENSE). Merely writing or running a program in ALK does not place that program under GPL-3.0; ALK programs may be distributed under whatever license their authors choose. This distinction does not exempt code that copies, modifies, or incorporates GPL-licensed implementation code from the applicable GPL requirements.
