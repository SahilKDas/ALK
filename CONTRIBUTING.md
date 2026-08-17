# Contributing to ALK

Before contributing, read [the provenance policy](PROVENANCE.md). Imported or derived code is not allowed unless it is CC0, Unlicensed, or covered by a recorded exception approved explicitly by the project owner.

Build and test every change:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Keep changes small, dependency-free, and accompanied by tests. New syntax must be documented with its current status, and planned features must not be described as implemented.
