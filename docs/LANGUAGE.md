# ALK language design and implementation status

This document records the intended language. A status label distinguishes executable behavior from design work:

- **Implemented**: accepted by the current 0.1 foundation interpreter and covered by tests.
- **Specified**: intended semantics, not implemented yet.
- **Exploratory**: direction that still requires a design decision or prototype.

## Principles

ALK is a dynamically typed web scripting language. It borrows the ergonomic object model, blocks, iterators, and mixins of Ruby 2.6 while requiring Python-like explicit receivers and predictable lexical scope. Ruby 3-only features such as pattern matching and Ractors are outside the language baseline.

The core language should remain small. Browser functionality belongs in explicit standard-library modules rather than implicit globals.

## Lexical and value core — Implemented

- `#` begins a line comment.
- Identifiers are ASCII letters or `_`, followed by letters, digits, or `_`; method names may end in `?` or `!`.
- Double-quoted strings support `#{expression}` interpolation. Single-quoted strings do not interpolate.
- Native values are `nil`, `true`, `false`, `Int64`, `Float64`, UTF-8 `String`, `Array`, and string-keyed `Map`.
- Operators currently include `+ - * / %`, comparisons, equality, and `and or not`.
- Assignment creates or replaces a name in the runtime's current scope.
- Only `nil` and `false` are falsey.

## JSON — Implemented

Every valid JSON document is accepted by `alk::parse_json` and `ALK.parse_json`. JSON `null` becomes ALK `nil`; ALK also accepts `null` as a literal alias so JSON text is valid source syntax. Serialization emits strict, compact JSON and translates `nil` back to `null`.

| JSON | ALK | Host representation |
| --- | --- | --- |
| object | `Map` | `alk::Value::Map` |
| array | `Array` | `alk::Value::Array` |
| string | `String` | UTF-8 `std::string` |
| integer | `Int64` when in range | `std::int64_t` |
| other number | `Float64` | `double` |
| boolean | Boolean | `bool` |
| null | `NilClass` / `nil` | empty value state |

Strict JSON parsing supports escapes, UTF-16 `\uXXXX` sequences, surrogate pairs, exponents, and structural validation. ALK map literals currently require quoted string keys.

## Functions, classes, and blocks — Specified

```alk
class TodoApp < WebComponent
  def initialize(self, root)
    super(root)
    self.todos = []
  end

  def active_todos(self)
    self.todos.select do |todo|
      not todo["completed"]
    end
  end
end
```

- Instance methods explicitly declare `self` as the first parameter.
- Lexical locals never silently become properties; `self.name` is required for property access.
- Blocks use `do |args| ... end` or `{ |args| ... }` and are first-class callable values.
- Non-local control flow from blocks must be defined precisely before implementation.
- Inline conditionals use `expression if condition` and `expression unless condition`.
- Comprehensions use `[expression for name in iterable if condition]`.
- Endless ranges use `start..` and must remain lazy.

## Modules and mixins — Specified

```alk
module Observable
  def emit(self, event, data)
    self.listeners[event]?.each { |callback| callback(data) }
  end
end

class Store
  include Observable
end
```

ALK uses single class inheritance plus module inclusion and prepension. Method lookup order and conflict rules will be frozen before the class runtime is implemented.

## Imports and browser APIs — Specified

```alk
import web.dom { Document, Element }
import web.http { Fetch }
```

Imports are explicit and statically resolvable. Browser bindings live under `web.*`. DOM objects are intended to be opaque host objects, not maps and not JavaScript proxies.

Declaring `<script type="application/alk">` does not make current browsers execute ALK. That MIME type requires either a browser-engine integration or a loader that explicitly locates ALK scripts. Native integration and a WebAssembly fallback are separate deliverables.

## Concurrency — Specified

Async host operations suspend lightweight ALK Fibers. `.await()` yields the current Fiber to the host event loop and resumes it with a value or exception. The design avoids forcing Promise-style syntax into user code, but cancellation, structured concurrency, and error propagation still require formal semantics.
