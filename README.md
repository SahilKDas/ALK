# ALK

ALK is a dynamically typed scripting language designed as a first-class web language. Its design combines Ruby 2.6-style blocks and mixins with explicit `self`, predictable scope, native JSON-compatible literals, and direct browser API bindings.

The repository currently contains the **0.1 foundation interpreter**, not the planned JIT or browser integration. It is a small, dependency-free C++23 implementation used to lock down ALK's value model, syntax, diagnostics, embedding API, and JSON behavior before bytecode and JIT work begins.

## Implemented today

- `nil`, booleans, signed 64-bit integers, doubles, UTF-8 strings, arrays, and string-keyed maps
- variables, comments, arithmetic, comparisons, `and` / `or` / `not`
- array, map, and string indexing
- double-quoted `#{...}` interpolation
- `puts`, `append`, `length`, `size`, `strip`, `empty?`, and `to_json`
- strict `ALK.parse_json(...)` and `alk::parse_json(...)`
- persistent runtime scope and line/column diagnostics
- a public C++23 `alk::Runtime` / `alk::Value` host API

Classes, methods, blocks, modules, imports, Fibers, bytecode, garbage collection, DOM bindings, WebAssembly, and JIT compilation are specified but not implemented yet. See [Language status](LANGUAGE.md) and [Architecture](ARCHITECTURE.md).

## Build

Requirements: CMake 3.25+ and a C++23 compiler. No package manager or third-party library is used.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a file or a short expression:

```sh
build/alk examples/hello.alk
build/alk -e 'puts {"answer": 42}["answer"]'
```

On a multi-config generator, the executable may be under `build/Release/`.

## Small example

```alk
name = "ALK"
data = {
  "language": name,
  "features": ["native JSON", "interpolation"]
}

puts "Hello from #{data['language']}!"
puts data.to_json()
```

## Project rules

ALK is developed as an original implementation. Imported code is prohibited unless it is released under CC0 or the Unlicense, or the project owner gives explicit written permission for a specific exception. See [Provenance policy](PROVENANCE.md) before contributing.

ALK itself is licensed under [GPL-3.0](LICENSE).
