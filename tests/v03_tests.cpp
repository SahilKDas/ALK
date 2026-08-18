#include <alk/alk.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

alk::Value run(alk::Runtime& runtime, std::string_view source) {
    auto result = runtime.execute_script(source);
    if (!result) {
        ++failures;
        std::cerr << "UNEXPECTED ERROR at " << result.error().line << ':' << result.error().column
                  << ": " << result.error().message << '\n';
        return {};
    }
    return std::move(*result);
}

std::int64_t map_integer(const alk::Value& map, std::string_view name) {
    const auto found = map.as_map().find(name);
    return found->second.as_int();
}

void test_native_jit_and_bytecode() {
    alk::Runtime runtime;
    const auto answer = run(runtime, R"(
def mul_add(a, b, c)
  return a * b + c
end
mul_add(6, 7, 1)
)");
    check(answer == alk::Value(std::int64_t{43}), "JIT-eligible function result");
    const auto stats = run(runtime, "ALK.jit_stats()");
    check(stats.is_map(), "JIT statistics are observable");
    check(map_integer(stats, "bytecode_functions") >= 1, "function compiled to verified bytecode");
    check(map_integer(stats, "bytecode_instructions") == 0,
          "native call bypassed bytecode dispatch");
    check(map_integer(stats, "jit_compilations") >= 1, "native code was compiled at runtime");
    if (stats.as_map().at("native_available").as_bool()) {
        check(map_integer(stats, "native_calls") >= 1, "native machine code was executed");
    }

    auto overflow = runtime.execute_script(R"(
def overflowing_add(a, b)
  return a + b
end
overflowing_add(9223372036854775807, 1)
)");
    check(!overflow, "JIT overflow deoptimizes to checked bytecode failure");
}

void test_functions_closures_and_control_flow() {
    alk::Runtime runtime;
    const auto result = run(runtime, R"(
def make_adder(x)
  def add(y)
    return x + y
  end
  return add
end

def classify(value)
  return "negative" if value < 0
  if value == 0
    return "zero"
  else
    return "positive"
  end
end

adder = make_adder(10)
i = 0
while i < 3
  i = i + 1
end
[adder(5), classify(-1), classify(0), classify(1), i]
)");
    const auto& values = result.as_array();
    check(values[0] == alk::Value(std::int64_t{15}), "lexical closure captures parent scope");
    check(values[1] == alk::Value("negative") && values[2] == alk::Value("zero") &&
          values[3] == alk::Value("positive"), "if, else, and postfix if");
    check(values[4] == alk::Value(std::int64_t{3}), "while loop");
}

void test_objects_inheritance_and_mixins() {
    alk::Runtime runtime;
    const auto result = run(runtime, R"(
module Incrementable
  def inc(self)
    self.value = self.value + 1
    return self.value
  end
end

class Base
  def initialize(self, value)
    self.value = value
  end
  def label(self)
    return "base"
  end
end

class Counter < Base
  include Incrementable
  def initialize(self, value)
    super(value)
  end
end

counter = Counter.new(4)
[counter.inc(), counter.label(), counter?.value, nil?.missing]
)");
    const auto& values = result.as_array();
    check(values[0] == alk::Value(std::int64_t{5}), "included module method");
    check(values[1] == alk::Value("base"), "single inheritance method lookup");
    check(values[2] == alk::Value(std::int64_t{5}), "explicit self property and safe navigation");
    check(values[3].is_nil(), "safe navigation on nil");

    const auto prepended = run(runtime, R"(
module Override
  def label(self)
    return "prepended"
  end
end
class WithOverride < Base
  prepend Override
end
WithOverride.new(1).label()
)");
    check(prepended == alk::Value("prepended"), "prepended module precedes class ancestry");

    auto invalid_self = runtime.execute_script(R"(
class Invalid
  def method(value)
    value
  end
end
)");
    check(!invalid_self, "instance methods require explicit self");
}

void test_blocks_ranges_comprehensions_and_exceptions() {
    alk::Runtime runtime;
    const auto result = run(runtime, R"(
values = [1, 2, 3, 4]
doubled = values.map do |value|
  value * 2
end
selected = doubled.select do |value|
  value > 4
end
sum = values.reduce(0) do |total, value|
  total + value
end
squares = [value * value for value in values if value > 2]
range_values = (7..).take(3)
message = "missing"
begin
  raise "handled"
rescue error
  message = error
end
[selected, sum, squares, range_values, message]
)");
    const auto& values = result.as_array();
    check(values[0].to_json() == "[6,8]", "map and select blocks");
    check(values[1] == alk::Value(std::int64_t{10}), "reduce block");
    check(values[2].to_json() == "[9,16]", "filtered list comprehension");
    check(values[3].to_json() == "[7,8,9]", "lazy endless range with finite take");
    check(values[4] == alk::Value("handled"), "raise and rescue");

    const auto yielded = run(runtime, R"(
def apply(value)
  return yield(value)
end
apply(5) do |value|
  value * 3
end
)");
    check(yielded == alk::Value(std::int64_t{15}), "function yield invokes supplied block");
}

void test_modules_and_collector() {
    alk::Runtime runtime;
    const auto parsed = run(runtime, R"(
import std.json { parse_json }
parse_json("{\"ok\":true}")["ok"]
)");
    check(parsed == alk::Value(true), "deterministic standard module import");
    check(!runtime.execute_script("import filesystem { read }"), "unknown module is rejected");

    const auto collected = run(runtime, R"(
class Temporary
  def initialize(self)
    self.value = 1
  end
end
def allocate_temporary()
  temporary = Temporary.new()
  return nil
end
allocate_temporary()
ALK.collect_garbage()
)");
    check(collected.is_int() && collected.as_int() >= 1, "precise heap collector reclaims unreachable object");
}

} // namespace

int main() {
    test_native_jit_and_bytecode();
    test_functions_closures_and_control_flow();
    test_objects_inheritance_and_mixins();
    test_blocks_ranges_comprehensions_and_exceptions();
    test_modules_and_collector();
    if (failures != 0) {
        std::cerr << failures << " v0.3 test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All ALK v0.2/v0.3/JIT tests passed\n";
    return EXIT_SUCCESS;
}
