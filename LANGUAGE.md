# ALK language reference

This document describes behavior implemented by the 0.3 development runtime. Browser APIs, Fibers, WebAssembly hosting, and the optimizing JIT remain future work.

## Values and truth

ALK is dynamically typed. Its core values are:

- `nil` (with `null` accepted as a JSON-compatible alias);
- `true` and `false`;
- signed `Int64` and `Float64` numbers;
- UTF-8 strings;
- arrays and string-keyed maps;
- functions, blocks, classes, modules, objects, and ranges.

Only `nil` and `false` are falsey.

## Variables and control flow

```alk
total = 0
i = 0

while i < 4
  total = total + i
  i = i + 1
end

if total > 5
  puts "large"
else
  puts "small"
end
```

ALK supports block `if` and `unless`, `else`, `while`, and postfix conditions:

```alk
return nil if title.strip().empty?
puts "ready" unless stopped
```

Assignments are local to the current function scope. Reads walk outward through captured lexical scopes.

## Functions and closures

```alk
def make_adder(amount)
  def add(value)
    return value + amount
  end
  return add
end

add_ten = make_adder(10)
puts add_ten(5)
```

Functions capture their defining lexical scope. `return` exits the current function. `raise value` raises a language exception, and `begin` / `rescue` handles it:

```alk
begin
  raise "problem"
rescue error
  puts error
end
```

## Classes, inheritance, and explicit self

Instance methods must declare `self` as their first parameter. Properties must be accessed through an explicit receiver.

```alk
class Counter
  def initialize(self, start)
    self.value = start
  end

  def increment(self)
    self.value = self.value + 1
    return self.value
  end
end

counter = Counter.new(4)
puts counter.increment()
```

Classes support single inheritance and receiver-preserving `super`:

```alk
class NamedCounter < Counter
  def initialize(self, start)
    super(start)
    self.name = "counter"
  end
end
```

Nil-safe navigation returns `nil` without evaluating a property or method on a nil receiver:

```alk
label = maybe_node?.label
```

## Modules and mixins

```alk
module Observable
  def emit(self, event)
    puts event
  end
end

class Store
  include Observable
end
```

`include` inserts module methods after class methods. `prepend` inserts module methods before class methods. Method lookup then continues through the parent class.

## Blocks, yield, and iterators

Blocks use Ruby-style explicit parameters:

```alk
doubled = [1, 2, 3].map do |value|
  value * 2
end
```

Implemented array iterators are `each`, `map`, `select`, and `reduce`. A function can invoke its supplied block with `yield(...)` and inspect `block_given?`.

```alk
def apply(value)
  return yield(value)
end

result = apply(5) do |value|
  value * 3
end
```

## Ranges and comprehensions

Inclusive ranges use `..`; exclusive ranges use `...`. An omitted end creates a lazy endless range that requires a finite consumer such as `take`.

```alk
first_three = (7..).take(3)       # [7, 8, 9]
squares = [n * n for n in first_three if n > 7]
```

## JSON

Every valid JSON value is valid ALK source syntax. JSON `null` becomes ALK `nil`; serialization translates `nil` back to `null`.

```alk
data = {
  "user": "Alex",
  "roles": ["admin", "developer"],
  "active": true,
  "metadata": null
}

puts data.to_json()
```

Strict JSON text can be parsed directly or imported from the deterministic standard module:

```alk
import std.json { parse_json }
parsed = parse_json("{\"answer\":42}")
```

The parser supports JSON escapes, Unicode escapes and surrogate pairs, exponents, and structural validation.

## Deterministic imports

```alk
import std.json { parse_json }
import std.math { abs }
```

Imports resolve through a fixed runtime registry. They do not search or execute arbitrary local files. The current exports are `std.json.parse_json` and `std.math.abs`.

## Execution tiers

Every function is compiled to verified register bytecode and executes in a VM call frame. Straight-line integer arithmetic functions are JIT-compiled to x86-64 machine code on first eligible invocation. Type guards and checked-overflow reporting fall back to bytecode when native assumptions do not hold.

```alk
def mul_add(a, b, c)
  return a * b + c
end

puts mul_add(6, 7, 1)
puts ALK.jit_stats().to_json()
```

Complex constructs currently execute through an explicit verified AST-fallback bytecode instruction. They remain inside the VM call-frame and root-tracking model but are not yet individually lowered or native-compiled.

## Browser status

Current browsers do not execute `<script type="application/alk">`. Native browser scripts require the future WebAssembly loader or a browser-engine integration. DOM, Fetch, Crypto, events, timers, and Fiber scheduling are planned for 0.4.
