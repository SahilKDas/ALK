#include "runtime_v2_internal.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace alk::detail {
namespace {

[[noreturn]] void runtime_error(SourceLocation location, std::string message) {
    throw RuntimeFailure{{std::move(message), location.line, location.column}};
}

std::shared_ptr<DynamicArray> make_array(std::vector<Dynamic> values = {}) {
    auto result = std::make_shared<DynamicArray>();
    result->values = std::move(values);
    return result;
}

std::shared_ptr<DynamicMap> make_map(
    std::map<std::string, Dynamic, std::less<>> values = {}) {
    auto result = std::make_shared<DynamicMap>();
    result->values = std::move(values);
    return result;
}

const std::shared_ptr<DynamicArray>& as_array(const Dynamic& value) {
    return std::get<std::shared_ptr<DynamicArray>>(value.storage);
}

const std::shared_ptr<DynamicMap>& as_map(const Dynamic& value) {
    return std::get<std::shared_ptr<DynamicMap>>(value.storage);
}

const std::string& as_string(const Dynamic& value) {
    return std::get<std::string>(value.storage);
}

ObjectRef as_object(const Dynamic& value) {
    return std::get<ObjectRef>(value.storage);
}

std::int64_t checked_integer(long double value, SourceLocation location) {
    constexpr long double minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    constexpr long double maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (value < minimum || value > maximum) runtime_error(location, "integer overflow");
    return static_cast<std::int64_t>(value);
}

Dynamic numeric_binary(std::string_view operation, const Dynamic& left, const Dynamic& right,
                       SourceLocation location) {
    if (!left.is_number() || !right.is_number()) {
        if (operation == "+" && left.is_string() && right.is_string()) {
            return Dynamic(as_string(left) + as_string(right));
        }
        runtime_error(location, "operator '" + std::string(operation) + "' requires numeric operands");
    }
    if (operation == "/" || operation == "%") {
        if (right.number() == 0.0) runtime_error(location, "division by zero");
    }
    if (left.is_int() && right.is_int() && operation != "/") {
        const auto lhs = left.integer();
        const auto rhs = right.integer();
        if (operation == "+") return Dynamic(checked_integer(static_cast<long double>(lhs) + rhs, location));
        if (operation == "-") return Dynamic(checked_integer(static_cast<long double>(lhs) - rhs, location));
        if (operation == "*") return Dynamic(checked_integer(static_cast<long double>(lhs) * rhs, location));
        if (operation == "%") {
            if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1) return Dynamic(std::int64_t{0});
            return Dynamic(lhs % rhs);
        }
    }
    const double lhs = left.number();
    const double rhs = right.number();
    if (operation == "+") return Dynamic(lhs + rhs);
    if (operation == "-") return Dynamic(lhs - rhs);
    if (operation == "*") return Dynamic(lhs * rhs);
    if (operation == "/") return Dynamic(lhs / rhs);
    if (operation == "%") return Dynamic(std::fmod(lhs, rhs));
    runtime_error(location, "unsupported numeric operator");
}

std::shared_ptr<Function> find_method(const std::shared_ptr<Class>& klass, std::string_view name) {
    if (!klass) return {};
    for (auto iterator = klass->prepended.rbegin(); iterator != klass->prepended.rend(); ++iterator) {
        const auto found = (*iterator)->methods.find(name);
        if (found != (*iterator)->methods.end()) return found->second;
    }
    if (const auto found = klass->methods.find(name); found != klass->methods.end()) return found->second;
    for (auto iterator = klass->included.rbegin(); iterator != klass->included.rend(); ++iterator) {
        const auto found = (*iterator)->methods.find(name);
        if (found != (*iterator)->methods.end()) return found->second;
    }
    return find_method(klass->parent, name);
}

Dynamic call_dynamic(RuntimeState& state, const std::shared_ptr<Scope>& scope,
                     const Dynamic& callee, std::vector<Dynamic> arguments,
                     const std::shared_ptr<BlockValue>& block, SourceLocation location);

Dynamic call_function(RuntimeState& state, const std::shared_ptr<Function>& function,
                      std::vector<Dynamic> arguments, const std::shared_ptr<BlockValue>& block,
                      SourceLocation location) {
    if (!function) runtime_error(location, "attempted to call an empty function");
    ++state.statistics.function_calls;
    if (function->bound_receiver) {
        arguments.insert(arguments.begin(), *function->bound_receiver);
    }

    if (function->name == "__builtin::parse_json") {
        if (arguments.size() != 1 || !arguments[0].is_string()) {
            runtime_error(location, "parse_json expects one string");
        }
        auto parsed = alk::parse_json(as_string(arguments[0]));
        if (!parsed) throw RuntimeFailure{parsed.error()};
        return from_public_value(*parsed);
    }
    if (function->name == "__builtin::abs") {
        if (arguments.size() != 1 || !arguments[0].is_number()) {
            runtime_error(location, "abs expects one number");
        }
        if (arguments[0].is_int()) {
            const auto value = arguments[0].integer();
            if (value == std::numeric_limits<std::int64_t>::min()) {
                runtime_error(location, "integer overflow in abs");
            }
            return Dynamic(value < 0 ? -value : value);
        }
        return Dynamic(std::fabs(arguments[0].number()));
    }

    if (arguments.size() != function->parameters.size()) {
        runtime_error(location, "function '" + function->name + "' expects " +
                                    std::to_string(function->parameters.size()) + " argument(s), got " +
                                    std::to_string(arguments.size()));
    }

    if (function->bytecode) {
        if (!function->native_code && native_available()) {
            function->native_code = compile_native(*function->bytecode);
            if (function->native_code) ++state.statistics.jit_compilations;
        }
        if (function->native_code) {
            if (auto result = invoke_native(function->native_code, arguments)) {
                ++state.statistics.native_calls;
                return Dynamic(*result);
            }
            ++state.statistics.jit_fallbacks;
        }
    }

    auto local = std::make_shared<Scope>();
    local->parent = function->closure;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        local->define(function->parameters[index], arguments[index]);
    }
    if (block) local->define("__block__", Dynamic(block));
    if (function->method && !arguments.empty()) {
        if (const auto owner = function->owner_class.lock(); owner && owner->parent) {
            if (const auto parent_method = find_method(owner->parent, function->name)) {
                auto bound_super = std::make_shared<Function>(*parent_method);
                bound_super->bound_receiver = arguments.front();
                local->define("super", Dynamic(bound_super));
            }
        }
    }
    state.active_scopes.push_back(local);
    try {
        Dynamic result;
        if (function->bytecode) result = execute_bytecode(state, *function->bytecode, local, arguments);
        else result = execute_statements(state, local, function->body, block);
        state.active_scopes.pop_back();
        return result;
    } catch (const ReturnSignal& returned) {
        state.active_scopes.pop_back();
        return returned.value;
    } catch (...) {
        state.active_scopes.pop_back();
        throw;
    }
}

std::shared_ptr<BlockValue> block_from_expression(const ExprPtr& expression,
                                                  const std::shared_ptr<Scope>& scope) {
    if (!expression || expression->block_body.empty()) return {};
    auto block = std::make_shared<BlockValue>();
    block->parameters = expression->block_parameters;
    block->body = expression->block_body;
    block->closure = scope;
    return block;
}

Dynamic call_block(RuntimeState& state, const std::shared_ptr<BlockValue>& block,
                   const std::vector<Dynamic>& arguments, SourceLocation location) {
    if (!block) runtime_error(location, "no block was provided");
    if (arguments.size() != block->parameters.size()) {
        runtime_error(location, "block expects " + std::to_string(block->parameters.size()) +
                                " argument(s), got " + std::to_string(arguments.size()));
    }
    auto local = std::make_shared<Scope>();
    local->parent = block->closure;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        local->define(block->parameters[index], arguments[index]);
    }
    state.active_scopes.push_back(local);
    try {
        Dynamic result = execute_statements(state, local, block->body);
        state.active_scopes.pop_back();
        return result;
    } catch (...) {
        state.active_scopes.pop_back();
        throw;
    }
}

std::vector<Dynamic> iterable_values(RuntimeState& state, const Dynamic& receiver,
                                     SourceLocation location, std::optional<std::size_t> limit = {}) {
    static_cast<void>(state);
    if (receiver.is_array()) return as_array(receiver)->values;
    if (receiver.is_map()) {
        std::vector<Dynamic> result;
        for (const auto& [key, value] : as_map(receiver)->values) {
            result.push_back(Dynamic(make_array({Dynamic(key), value})));
        }
        return result;
    }
    if (receiver.is_range()) {
        const auto range = std::get<std::shared_ptr<RangeValue>>(receiver.storage);
        if (!range->last && !limit) runtime_error(location, "endless range requires a finite consumer such as take");
        std::vector<Dynamic> result;
        const std::size_t maximum = limit.value_or(std::numeric_limits<std::size_t>::max());
        std::int64_t value = range->first;
        while (result.size() < maximum) {
            if (range->last) {
                if (range->exclusive ? value >= *range->last : value > *range->last) break;
            }
            result.emplace_back(value);
            if (value == std::numeric_limits<std::int64_t>::max()) break;
            ++value;
        }
        return result;
    }
    runtime_error(location, "value of type " + receiver.type_name() + " is not iterable");
}

Dynamic call_method(RuntimeState& state, const std::shared_ptr<Scope>& scope,
                    const Dynamic& receiver, std::string_view name,
                    std::vector<Dynamic> arguments, const std::shared_ptr<BlockValue>& block,
                    SourceLocation location) {
    static_cast<void>(scope);
    if (receiver.is_nil()) runtime_error(location, "cannot call method on nil");

    if (receiver.is_object()) {
        Object& object = state.heap.get(as_object(receiver));
        if (const auto method = find_method(object.klass, name)) {
            arguments.insert(arguments.begin(), receiver);
            return call_function(state, method, std::move(arguments), block, location);
        }
        runtime_error(location, "undefined method '" + std::string(name) + "' for " + object.klass->name);
    }

    if (receiver.is_class() && name == "new") {
        const auto klass = std::get<std::shared_ptr<Class>>(receiver.storage);
        const ObjectRef reference = state.heap.allocate(klass);
        const Dynamic object(reference);
        if (const auto initializer = find_method(klass, "initialize")) {
            arguments.insert(arguments.begin(), object);
            static_cast<void>(call_function(state, initializer, std::move(arguments), {}, location));
        } else if (!arguments.empty()) {
            runtime_error(location, "class '" + klass->name + "' has no initializer accepting arguments");
        }
        return object;
    }

    if (receiver.is_array()) {
        auto array = as_array(receiver);
        if (name == "append") {
            if (arguments.size() != 1) runtime_error(location, "append expects one argument");
            array->values.push_back(arguments[0]);
            return receiver;
        }
        if (name == "length" || name == "size") {
            if (!arguments.empty()) runtime_error(location, "length expects no arguments");
            return Dynamic(static_cast<std::int64_t>(array->values.size()));
        }
        if (name == "empty?") return Dynamic(array->values.empty());
        if (name == "each" || name == "map" || name == "select" || name == "reduce") {
            if (!block) runtime_error(location, std::string(name) + " requires a block");
            if (name == "reduce") {
                Dynamic accumulator;
                std::size_t start = 0;
                if (!arguments.empty()) accumulator = arguments[0];
                else if (!array->values.empty()) { accumulator = array->values[0]; start = 1; }
                else return Dynamic{};
                for (std::size_t index = start; index < array->values.size(); ++index) {
                    accumulator = call_block(state, block, {accumulator, array->values[index]}, location);
                }
                return accumulator;
            }
            std::vector<Dynamic> output;
            for (const Dynamic& value : array->values) {
                Dynamic transformed = call_block(state, block, {value}, location);
                if (name == "map") output.push_back(std::move(transformed));
                else if (name == "select" && transformed.truthy()) output.push_back(value);
            }
            return name == "each" ? receiver : Dynamic(make_array(std::move(output)));
        }
    }

    if (receiver.is_map()) {
        const auto map = as_map(receiver);
        if (name == "length" || name == "size") return Dynamic(static_cast<std::int64_t>(map->values.size()));
        if (name == "empty?") return Dynamic(map->values.empty());
        if (name == "keys") {
            std::vector<Dynamic> keys;
            for (const auto& [key, value] : map->values) { static_cast<void>(value); keys.emplace_back(key); }
            return Dynamic(make_array(std::move(keys)));
        }
    }

    if (receiver.is_string()) {
        const auto& string = as_string(receiver);
        if (name == "length" || name == "size") return Dynamic(static_cast<std::int64_t>(string.size()));
        if (name == "empty?") return Dynamic(string.empty());
        if (name == "strip") {
            const auto first = string.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return Dynamic("");
            const auto last = string.find_last_not_of(" \t\r\n");
            return Dynamic(string.substr(first, last - first + 1));
        }
    }

    if (receiver.is_range() && name == "take") {
        if (arguments.size() != 1 || !arguments[0].is_int() || arguments[0].integer() < 0) {
            runtime_error(location, "take expects one non-negative integer");
        }
        return Dynamic(make_array(iterable_values(
            state, receiver, location, static_cast<std::size_t>(arguments[0].integer()))));
    }

    if (receiver.is_block() && name == "call") {
        return call_block(state, std::get<std::shared_ptr<BlockValue>>(receiver.storage), arguments, location);
    }

    if (name == "to_json") {
        if (!arguments.empty()) runtime_error(location, "to_json expects no arguments");
        return Dynamic(to_public_value(state, receiver).to_json());
    }

    if (receiver.is_alk_namespace()) {
        if (name == "parse_json") {
            auto function = std::make_shared<Function>();
            function->name = "__builtin::parse_json";
            return call_function(state, function, std::move(arguments), block, location);
        }
        if (name == "jit_stats") {
            if (!arguments.empty()) runtime_error(location, "jit_stats expects no arguments");
            const auto& stats = state.statistics;
            return Dynamic(make_map({
                {"bytecode_functions", Dynamic(static_cast<std::int64_t>(stats.bytecode_functions))},
                {"bytecode_instructions", Dynamic(static_cast<std::int64_t>(stats.bytecode_instructions))},
                {"collections", Dynamic(static_cast<std::int64_t>(stats.collections))},
                {"jit_compilations", Dynamic(static_cast<std::int64_t>(stats.jit_compilations))},
                {"jit_fallbacks", Dynamic(static_cast<std::int64_t>(stats.jit_fallbacks))},
                {"native_available", Dynamic(native_available())},
                {"native_calls", Dynamic(static_cast<std::int64_t>(stats.native_calls))},
                {"objects_collected", Dynamic(static_cast<std::int64_t>(stats.objects_collected))}
            }));
        }
        if (name == "collect_garbage") {
            auto roots = state.active_scopes;
            roots.push_back(state.globals);
            const std::size_t collected = state.heap.collect(roots, {});
            ++state.statistics.collections;
            state.statistics.objects_collected += collected;
            return Dynamic(static_cast<std::int64_t>(collected));
        }
    }

    runtime_error(location, "unsupported method '" + std::string(name) + "' for " + receiver.type_name());
}

Dynamic call_dynamic(RuntimeState& state, const std::shared_ptr<Scope>& scope,
                     const Dynamic& callee, std::vector<Dynamic> arguments,
                     const std::shared_ptr<BlockValue>& block, SourceLocation location) {
    static_cast<void>(scope);
    if (callee.is_function()) {
        return call_function(state, std::get<std::shared_ptr<Function>>(callee.storage),
                             std::move(arguments), block, location);
    }
    if (callee.is_block()) {
        return call_block(state, std::get<std::shared_ptr<BlockValue>>(callee.storage), arguments, location);
    }
    runtime_error(location, "value of type " + callee.type_name() + " is not callable");
}

Dynamic index_value(RuntimeState& state, const Dynamic& receiver, const Dynamic& index,
                    SourceLocation location) {
    static_cast<void>(state);
    if (receiver.is_array()) {
        if (!index.is_int()) runtime_error(location, "array index must be an integer");
        const auto raw = index.integer();
        const auto& values = as_array(receiver)->values;
        if (raw < 0 || static_cast<std::uint64_t>(raw) >= values.size()) {
            runtime_error(location, "array index out of bounds");
        }
        return values[static_cast<std::size_t>(raw)];
    }
    if (receiver.is_map()) {
        if (!index.is_string()) runtime_error(location, "map index must be a string");
        const auto found = as_map(receiver)->values.find(as_string(index));
        return found == as_map(receiver)->values.end() ? Dynamic{} : found->second;
    }
    if (receiver.is_string()) {
        if (!index.is_int()) runtime_error(location, "string index must be an integer");
        const auto raw = index.integer();
        if (raw < 0 || static_cast<std::uint64_t>(raw) >= as_string(receiver).size()) {
            runtime_error(location, "string index out of bounds");
        }
        return Dynamic(std::string(1, as_string(receiver)[static_cast<std::size_t>(raw)]));
    }
    runtime_error(location, "value of type " + receiver.type_name() + " is not indexable");
}

void assign_index(const Dynamic& receiver, const Dynamic& index, Dynamic value,
                  SourceLocation location) {
    if (receiver.is_array()) {
        if (!index.is_int()) runtime_error(location, "array index must be an integer");
        const auto raw = index.integer();
        auto& values = as_array(receiver)->values;
        if (raw < 0 || static_cast<std::uint64_t>(raw) >= values.size()) {
            runtime_error(location, "array index out of bounds");
        }
        values[static_cast<std::size_t>(raw)] = std::move(value);
        return;
    }
    if (receiver.is_map()) {
        if (!index.is_string()) runtime_error(location, "map index must be a string");
        as_map(receiver)->values.insert_or_assign(as_string(index), std::move(value));
        return;
    }
    runtime_error(location, "value is not assignable by index");
}

std::string interpolate(RuntimeState& state, const std::shared_ptr<Scope>& scope,
                        std::string_view input, SourceLocation location) {
    std::string output;
    std::size_t cursor = 0;
    while (cursor < input.size()) {
        const auto start = input.find("#{", cursor);
        if (start == std::string_view::npos) {
            output.append(input.substr(cursor));
            break;
        }
        output.append(input.substr(cursor, start - cursor));
        std::size_t end = start + 2;
        int depth = 1;
        char quote = '\0';
        for (; end < input.size(); ++end) {
            const char ch = input[end];
            if (quote != '\0') {
                if (ch == quote && (end == 0 || input[end - 1] != '\\')) quote = '\0';
                continue;
            }
            if (ch == '"' || ch == '\'') quote = ch;
            else if (ch == '{') ++depth;
            else if (ch == '}' && --depth == 0) break;
        }
        if (depth != 0) runtime_error(location, "unterminated interpolation");
        auto parsed = parse_program(input.substr(start + 2, end - start - 2));
        if (!parsed) throw RuntimeFailure{parsed.error()};
        output += inspect(state, execute_statements(state, scope, parsed->statements));
        cursor = end + 1;
    }
    return output;
}

std::shared_ptr<Function> make_function(RuntimeState& state, const StmtPtr& statement,
                                        const std::shared_ptr<Scope>& scope, bool method) {
    auto function = std::make_shared<Function>();
    function->name = statement->name;
    function->parameters = statement->names;
    function->body = statement->body;
    function->closure = scope;
    function->method = method;
    if (method && (function->parameters.empty() || function->parameters.front() != "self")) {
        runtime_error(statement->location, "instance method '" + function->name +
                                           "' must declare self as its first parameter");
    }
    auto bytecode = compile_function_bytecode(function->name, function->parameters, function->body);
    ++state.statistics.verifier_runs;
    if (bytecode && verify_bytecode(*bytecode)) {
        function->bytecode = std::move(*bytecode);
        ++state.statistics.bytecode_functions;
    }
    return function;
}

void define_class(RuntimeState& state, const StmtPtr& statement,
                  const std::shared_ptr<Scope>& scope) {
    auto klass = std::make_shared<Class>();
    klass->name = statement->name;
    if (!statement->secondary_name.empty()) {
        const auto parent = scope->get(statement->secondary_name);
        if (!parent || !parent->is_class()) {
            runtime_error(statement->location, "unknown parent class '" + statement->secondary_name + "'");
        }
        klass->parent = std::get<std::shared_ptr<Class>>(parent->storage);
    }
    for (const auto& member : statement->body) {
        if (member->kind == StmtKind::function_definition) {
            auto method = make_function(state, member, scope, true);
            method->owner_class = klass;
            klass->methods.insert_or_assign(member->name, std::move(method));
        } else if (member->kind == StmtKind::include_module || member->kind == StmtKind::prepend_module) {
            const auto value = scope->get(member->name);
            if (!value || !value->is_module()) {
                runtime_error(member->location, "unknown module '" + member->name + "'");
            }
            const auto module = std::get<std::shared_ptr<Module>>(value->storage);
            if (member->kind == StmtKind::include_module) klass->included.push_back(module);
            else klass->prepended.push_back(module);
        } else {
            runtime_error(member->location, "class bodies may contain only methods, include, and prepend");
        }
    }
    scope->define(klass->name, Dynamic(klass));
}

void define_module(RuntimeState& state, const StmtPtr& statement,
                   const std::shared_ptr<Scope>& scope) {
    auto module = std::make_shared<Module>();
    module->name = statement->name;
    for (const auto& member : statement->body) {
        if (member->kind != StmtKind::function_definition) {
            runtime_error(member->location, "module bodies may contain only methods");
        }
        module->methods.insert_or_assign(member->name, make_function(state, member, scope, true));
    }
    scope->define(module->name, Dynamic(module));
}

} // namespace

bool Dynamic::is_nil() const noexcept { return std::holds_alternative<std::monostate>(storage); }
bool Dynamic::is_bool() const noexcept { return std::holds_alternative<bool>(storage); }
bool Dynamic::is_int() const noexcept { return std::holds_alternative<std::int64_t>(storage); }
bool Dynamic::is_float() const noexcept { return std::holds_alternative<double>(storage); }
bool Dynamic::is_number() const noexcept { return is_int() || is_float(); }
bool Dynamic::is_string() const noexcept { return std::holds_alternative<std::string>(storage); }
bool Dynamic::is_array() const noexcept { return std::holds_alternative<std::shared_ptr<DynamicArray>>(storage); }
bool Dynamic::is_map() const noexcept { return std::holds_alternative<std::shared_ptr<DynamicMap>>(storage); }
bool Dynamic::is_object() const noexcept { return std::holds_alternative<ObjectRef>(storage); }
bool Dynamic::is_function() const noexcept { return std::holds_alternative<std::shared_ptr<Function>>(storage); }
bool Dynamic::is_class() const noexcept { return std::holds_alternative<std::shared_ptr<Class>>(storage); }
bool Dynamic::is_module() const noexcept { return std::holds_alternative<std::shared_ptr<Module>>(storage); }
bool Dynamic::is_block() const noexcept { return std::holds_alternative<std::shared_ptr<BlockValue>>(storage); }
bool Dynamic::is_range() const noexcept { return std::holds_alternative<std::shared_ptr<RangeValue>>(storage); }
bool Dynamic::is_alk_namespace() const noexcept { return std::holds_alternative<AlkNamespace>(storage); }
bool Dynamic::truthy() const noexcept { return !is_nil() && (!is_bool() || std::get<bool>(storage)); }
double Dynamic::number() const { return is_int() ? static_cast<double>(integer()) : std::get<double>(storage); }
std::int64_t Dynamic::integer() const { return std::get<std::int64_t>(storage); }
std::string Dynamic::type_name() const {
    if (is_nil()) return "NilClass";
    if (is_bool()) return std::get<bool>(storage) ? "TrueClass" : "FalseClass";
    if (is_int()) return "Int64";
    if (is_float()) return "Float64";
    if (is_string()) return "String";
    if (is_array()) return "Array";
    if (is_map()) return "Map";
    if (is_object()) return "Object";
    if (is_function()) return "Function";
    if (is_class()) return "Class";
    if (is_module()) return "Module";
    if (is_block()) return "Block";
    if (is_range()) return "Range";
    return "ALK";
}

std::optional<Dynamic> Scope::get(std::string_view name) const {
    if (const auto found = values.find(name); found != values.end()) return found->second;
    return parent ? parent->get(name) : std::optional<Dynamic>{};
}
void Scope::define(std::string name, Dynamic value) { values.insert_or_assign(std::move(name), std::move(value)); }

ObjectRef ManagedHeap::allocate(std::shared_ptr<Class> klass) {
    std::size_t id;
    if (free_ids_.empty()) {
        id = objects_.size();
        objects_.push_back(std::make_unique<Object>());
    } else {
        id = free_ids_.back();
        free_ids_.pop_back();
        objects_[id] = std::make_unique<Object>();
    }
    objects_[id]->klass = std::move(klass);
    return {id};
}
Object& ManagedHeap::get(ObjectRef ref) {
    if (ref.id >= objects_.size() || !objects_[ref.id]) throw std::runtime_error("dangling object reference");
    return *objects_[ref.id];
}
const Object& ManagedHeap::get(ObjectRef ref) const {
    if (ref.id >= objects_.size() || !objects_[ref.id]) throw std::runtime_error("dangling object reference");
    return *objects_[ref.id];
}
std::size_t ManagedHeap::live_objects() const noexcept {
    return static_cast<std::size_t>(std::count_if(objects_.begin(), objects_.end(),
        [](const auto& object) { return static_cast<bool>(object); }));
}
void ManagedHeap::mark_scope(const std::shared_ptr<Scope>& scope, std::set<const Scope*>& visited) {
    if (!scope || !visited.insert(scope.get()).second) return;
    for (const auto& [name, value] : scope->values) { static_cast<void>(name); mark_dynamic(value, visited); }
    mark_scope(scope->parent, visited);
}
void ManagedHeap::mark_dynamic(const Dynamic& value, std::set<const Scope*>& visited) {
    if (value.is_object()) {
        const auto ref = as_object(value);
        if (ref.id >= objects_.size() || !objects_[ref.id] || objects_[ref.id]->marked) return;
        objects_[ref.id]->marked = true;
        for (const auto& [name, field] : objects_[ref.id]->fields) {
            static_cast<void>(name);
            mark_dynamic(field, visited);
        }
    } else if (value.is_array()) {
        for (const auto& element : as_array(value)->values) mark_dynamic(element, visited);
    } else if (value.is_map()) {
        for (const auto& [name, element] : as_map(value)->values) { static_cast<void>(name); mark_dynamic(element, visited); }
    } else if (value.is_function()) {
        mark_scope(std::get<std::shared_ptr<Function>>(value.storage)->closure, visited);
    } else if (value.is_block()) {
        mark_scope(std::get<std::shared_ptr<BlockValue>>(value.storage)->closure, visited);
    } else if (value.is_class()) {
        const auto klass = std::get<std::shared_ptr<Class>>(value.storage);
        for (const auto& [name, method] : klass->methods) { static_cast<void>(name); mark_scope(method->closure, visited); }
    } else if (value.is_module()) {
        const auto module = std::get<std::shared_ptr<Module>>(value.storage);
        for (const auto& [name, method] : module->methods) { static_cast<void>(name); mark_scope(method->closure, visited); }
    }
}
std::size_t ManagedHeap::collect(const std::vector<std::shared_ptr<Scope>>& roots,
                                 const std::vector<Dynamic>& values) {
    for (auto& object : objects_) if (object) object->marked = false;
    std::set<const Scope*> visited;
    for (const auto& scope : roots) mark_scope(scope, visited);
    for (const auto& value : values) mark_dynamic(value, visited);
    std::size_t collected = 0;
    for (std::size_t id = 0; id < objects_.size(); ++id) {
        if (objects_[id] && !objects_[id]->marked) {
            objects_[id].reset();
            free_ids_.push_back(id);
            ++collected;
        }
    }
    return collected;
}

Dynamic evaluate_expression(RuntimeState& state, const std::shared_ptr<Scope>& scope,
                            const ExprPtr& expression, const std::shared_ptr<BlockValue>& block) {
    if (!expression) return Dynamic{};
    switch (expression->kind) {
    case ExprKind::literal:
        if (expression->text == "interpolated") {
            return Dynamic(interpolate(state, scope, as_string(expression->literal), expression->location));
        }
        return expression->literal;
    case ExprKind::variable: {
        if (expression->text == "yield") {
            if (block) return Dynamic(block);
            const auto captured = scope->get("__block__");
            if (!captured || !captured->is_block()) {
                runtime_error(expression->location, "yield called without a block");
            }
            return *captured;
        }
        if (expression->text == "block_given?") {
            const auto captured = scope->get("__block__");
            return Dynamic(static_cast<bool>(block) || (captured && captured->is_block()));
        }
        const auto value = scope->get(expression->text);
        if (!value) runtime_error(expression->location, "undefined variable '" + expression->text + "'");
        return *value;
    }
    case ExprKind::array: {
        std::vector<Dynamic> values;
        for (const auto& item : expression->items) values.push_back(evaluate_expression(state, scope, item, block));
        return Dynamic(make_array(std::move(values)));
    }
    case ExprKind::map: {
        std::map<std::string, Dynamic, std::less<>> values;
        for (const auto& [key, value] : expression->entries) {
            values.insert_or_assign(key, evaluate_expression(state, scope, value, block));
        }
        return Dynamic(make_map(std::move(values)));
    }
    case ExprKind::unary: {
        Dynamic value = evaluate_expression(state, scope, expression->right, block);
        if (expression->text == "not" || expression->text == "!") return Dynamic(!value.truthy());
        if (!value.is_number()) runtime_error(expression->location, "unary minus requires a number");
        if (value.is_int()) {
            if (value.integer() == std::numeric_limits<std::int64_t>::min()) {
                runtime_error(expression->location, "integer overflow");
            }
            return Dynamic(-value.integer());
        }
        return Dynamic(-value.number());
    }
    case ExprKind::binary: {
        Dynamic left = evaluate_expression(state, scope, expression->left, block);
        if (expression->text == "and" && !left.truthy()) return Dynamic(false);
        if (expression->text == "or" && left.truthy()) return Dynamic(true);
        Dynamic right = evaluate_expression(state, scope, expression->right, block);
        if (expression->text == "and" || expression->text == "or") return Dynamic(right.truthy());
        if (expression->text == "==") return Dynamic(dynamic_equal(state, left, right));
        if (expression->text == "!=") return Dynamic(!dynamic_equal(state, left, right));
        if (expression->text == "+" || expression->text == "-" || expression->text == "*" ||
            expression->text == "/" || expression->text == "%") {
            return numeric_binary(expression->text, left, right, expression->location);
        }
        if (left.is_number() && right.is_number()) {
            if (expression->text == "<") return Dynamic(left.number() < right.number());
            if (expression->text == "<=") return Dynamic(left.number() <= right.number());
            if (expression->text == ">") return Dynamic(left.number() > right.number());
            if (expression->text == ">=") return Dynamic(left.number() >= right.number());
        }
        if (left.is_string() && right.is_string()) {
            if (expression->text == "<") return Dynamic(as_string(left) < as_string(right));
            if (expression->text == "<=") return Dynamic(as_string(left) <= as_string(right));
            if (expression->text == ">") return Dynamic(as_string(left) > as_string(right));
            if (expression->text == ">=") return Dynamic(as_string(left) >= as_string(right));
        }
        runtime_error(expression->location, "comparison requires compatible operands");
    }
    case ExprKind::index:
        return index_value(state,
            evaluate_expression(state, scope, expression->left, block),
            evaluate_expression(state, scope, expression->right, block), expression->location);
    case ExprKind::property:
    case ExprKind::safe_property: {
        Dynamic receiver = evaluate_expression(state, scope, expression->left, block);
        if (expression->kind == ExprKind::safe_property && receiver.is_nil()) return Dynamic{};
        if (receiver.is_object()) {
            Object& object = state.heap.get(as_object(receiver));
            if (const auto field = object.fields.find(expression->text); field != object.fields.end()) {
                return field->second;
            }
        }
        return call_method(state, scope, receiver, expression->text, {}, {}, expression->location);
    }
    case ExprKind::call: {
        std::vector<Dynamic> arguments;
        for (const auto& argument : expression->arguments) {
            arguments.push_back(evaluate_expression(state, scope, argument, block));
        }
        const auto supplied_block = block_from_expression(expression, scope);
        if (expression->left && (expression->left->kind == ExprKind::property ||
                                 expression->left->kind == ExprKind::safe_property)) {
            Dynamic receiver = evaluate_expression(state, scope, expression->left->left, block);
            if (expression->left->kind == ExprKind::safe_property && receiver.is_nil()) return Dynamic{};
            return call_method(state, scope, receiver, expression->left->text,
                               std::move(arguments), supplied_block, expression->location);
        }
        Dynamic callee = evaluate_expression(state, scope, expression->left, block);
        return call_dynamic(state, scope, callee, std::move(arguments), supplied_block, expression->location);
    }
    case ExprKind::range: {
        Dynamic first = evaluate_expression(state, scope, expression->left, block);
        if (!first.is_int()) runtime_error(expression->location, "range start must be an integer");
        auto range = std::make_shared<RangeValue>();
        range->first = first.integer();
        range->exclusive = expression->text == "...";
        if (expression->right) {
            Dynamic last = evaluate_expression(state, scope, expression->right, block);
            if (!last.is_int()) runtime_error(expression->location, "range end must be an integer");
            range->last = last.integer();
        }
        return Dynamic(range);
    }
    case ExprKind::comprehension: {
        Dynamic source = evaluate_expression(state, scope, expression->right, block);
        std::vector<Dynamic> output;
        for (const auto& value : iterable_values(state, source, expression->location)) {
            auto local = std::make_shared<Scope>();
            local->parent = scope;
            local->define(expression->text, value);
            if (expression->third && !evaluate_expression(state, local, expression->third, block).truthy()) continue;
            output.push_back(evaluate_expression(state, local, expression->left, block));
        }
        return Dynamic(make_array(std::move(output)));
    }
    }
    runtime_error(expression->location, "unknown expression");
}

Dynamic execute_statements(RuntimeState& state, const std::shared_ptr<Scope>& scope,
                           const StatementList& statements, const std::shared_ptr<BlockValue>& block) {
    Dynamic result;
    for (const auto& statement : statements) {
        switch (statement->kind) {
        case StmtKind::expression:
            result = evaluate_expression(state, scope, statement->expression, block);
            break;
        case StmtKind::print:
            result = evaluate_expression(state, scope, statement->expression, block);
            std::cout << inspect(state, result) << '\n';
            break;
        case StmtKind::assignment:
            result = evaluate_expression(state, scope, statement->expression, block);
            scope->define(statement->name, result);
            break;
        case StmtKind::property_assignment: {
            Dynamic receiver = evaluate_expression(state, scope, statement->receiver, block);
            if (!receiver.is_object()) runtime_error(statement->location, "property assignment requires an object");
            result = evaluate_expression(state, scope, statement->expression, block);
            state.heap.get(as_object(receiver)).fields.insert_or_assign(statement->name, result);
            break;
        }
        case StmtKind::index_assignment: {
            Dynamic receiver = evaluate_expression(state, scope, statement->receiver, block);
            Dynamic index = evaluate_expression(state, scope, statement->index, block);
            result = evaluate_expression(state, scope, statement->expression, block);
            assign_index(receiver, index, result, statement->location);
            break;
        }
        case StmtKind::return_value:
            throw ReturnSignal{statement->expression
                ? evaluate_expression(state, scope, statement->expression, block) : Dynamic{}};
        case StmtKind::function_definition: {
            auto function = make_function(state, statement, scope, false);
            result = Dynamic(function);
            scope->define(statement->name, result);
            break;
        }
        case StmtKind::class_definition:
            define_class(state, statement, scope);
            result = scope->get(statement->name).value();
            break;
        case StmtKind::module_definition:
            define_module(state, statement, scope);
            result = scope->get(statement->name).value();
            break;
        case StmtKind::include_module:
        case StmtKind::prepend_module:
            runtime_error(statement->location, "include and prepend are valid only inside a class");
        case StmtKind::raise_value:
            throw RaiseSignal{evaluate_expression(state, scope, statement->expression, block), statement->location};
        case StmtKind::rescue_block:
            try {
                result = execute_statements(state, scope, statement->body, block);
            } catch (const RaiseSignal& raised) {
                if (statement->rescue_body.empty()) throw;
                if (!statement->name.empty()) scope->define(statement->name, raised.value);
                result = execute_statements(state, scope, statement->rescue_body, block);
            } catch (const RuntimeFailure& failure) {
                if (statement->rescue_body.empty()) throw;
                if (!statement->name.empty()) scope->define(statement->name, Dynamic(failure.error.message));
                result = execute_statements(state, scope, statement->rescue_body, block);
            }
            break;
        case StmtKind::import_names: {
            const auto module = state.standard_modules.find(statement->name);
            if (module == state.standard_modules.end()) {
                runtime_error(statement->location, "unknown deterministic module '" + statement->name + "'");
            }
            for (const auto& name : statement->names) {
                const auto exported = module->second->methods.find(name);
                if (exported == module->second->methods.end()) {
                    runtime_error(statement->location, "module '" + statement->name +
                                                       "' does not export '" + name + "'");
                }
                scope->define(name, Dynamic(exported->second));
            }
            result = Dynamic(module->second);
            break;
        }
        case StmtKind::conditional: {
            const bool condition = evaluate_expression(state, scope, statement->expression, block).truthy();
            const bool take_body = statement->name == "unless" ? !condition : condition;
            result = execute_statements(state, scope,
                take_body ? statement->body : statement->rescue_body, block);
            break;
        }
        case StmtKind::while_loop: {
            std::size_t iterations = 0;
            while (evaluate_expression(state, scope, statement->expression, block).truthy()) {
                result = execute_statements(state, scope, statement->body, block);
                if (++iterations > 10'000'000U) {
                    runtime_error(statement->location, "loop iteration safety limit exceeded");
                }
            }
            break;
        }
        }
    }
    return result;
}

Value to_public_value(RuntimeState& state, const Dynamic& value) {
    if (value.is_nil()) return Value{};
    if (value.is_bool()) return Value(std::get<bool>(value.storage));
    if (value.is_int()) return Value(value.integer());
    if (value.is_float()) return Value(value.number());
    if (value.is_string()) return Value(as_string(value));
    if (value.is_array()) {
        Value::Array output;
        for (const auto& item : as_array(value)->values) output.push_back(to_public_value(state, item));
        return Value(std::move(output));
    }
    if (value.is_map()) {
        Value::Map output;
        for (const auto& [key, item] : as_map(value)->values) output.emplace(key, to_public_value(state, item));
        return Value(std::move(output));
    }
    if (value.is_object()) {
        Value::Map output;
        const Object& object = state.heap.get(as_object(value));
        output.emplace("__class__", Value(object.klass ? object.klass->name : "Object"));
        for (const auto& [key, item] : object.fields) output.emplace(key, to_public_value(state, item));
        return Value(std::move(output));
    }
    return Value(inspect(state, value));
}

Dynamic from_public_value(const Value& value) {
    if (value.is_nil()) return Dynamic{};
    if (value.is_bool()) return Dynamic(value.as_bool());
    if (value.is_int()) return Dynamic(value.as_int());
    if (value.is_float()) return Dynamic(value.as_float());
    if (value.is_string()) return Dynamic(value.as_string());
    if (value.is_array()) {
        std::vector<Dynamic> output;
        for (const auto& item : value.as_array()) output.push_back(from_public_value(item));
        return Dynamic(make_array(std::move(output)));
    }
    std::map<std::string, Dynamic, std::less<>> output;
    for (const auto& [key, item] : value.as_map()) output.emplace(key, from_public_value(item));
    return Dynamic(make_map(std::move(output)));
}

std::string inspect(RuntimeState& state, const Dynamic& value) {
    if (value.is_nil()) return "nil";
    if (value.is_bool()) return std::get<bool>(value.storage) ? "true" : "false";
    if (value.is_int()) return std::to_string(value.integer());
    if (value.is_float()) {
        std::ostringstream stream;
        stream << value.number();
        return stream.str();
    }
    if (value.is_string()) return as_string(value);
    if (value.is_function()) return "<function " + std::get<std::shared_ptr<Function>>(value.storage)->name + ">";
    if (value.is_class()) return "<class " + std::get<std::shared_ptr<Class>>(value.storage)->name + ">";
    if (value.is_module()) return "<module " + std::get<std::shared_ptr<Module>>(value.storage)->name + ">";
    if (value.is_block()) return "<block>";
    if (value.is_range()) {
        const auto range = std::get<std::shared_ptr<RangeValue>>(value.storage);
        return std::to_string(range->first) + (range->exclusive ? "..." : "..") +
               (range->last ? std::to_string(*range->last) : "");
    }
    if (value.is_alk_namespace()) return "ALK";
    return to_public_value(state, value).inspect();
}

bool dynamic_equal(RuntimeState& state, const Dynamic& left, const Dynamic& right) {
    if (left.is_number() && right.is_number()) return left.number() == right.number();
    if (left.storage.index() != right.storage.index()) return false;
    if (left.is_nil()) return true;
    if (left.is_bool()) return std::get<bool>(left.storage) == std::get<bool>(right.storage);
    if (left.is_string()) return as_string(left) == as_string(right);
    if (left.is_object()) return as_object(left) == as_object(right);
    if (left.is_array() || left.is_map()) return to_public_value(state, left) == to_public_value(state, right);
    return left.storage == right.storage;
}

AdvancedEngine::AdvancedEngine() : state_(std::make_unique<RuntimeState>()) {
    state_->globals->define("ALK", Dynamic(AlkNamespace{}));

    auto json = std::make_shared<Module>();
    json->name = "std.json";
    auto parse = std::make_shared<Function>();
    parse->name = "__builtin::parse_json";
    json->methods.emplace("parse_json", parse);
    state_->standard_modules.emplace(json->name, json);

    auto math = std::make_shared<Module>();
    math->name = "std.math";
    auto absolute = std::make_shared<Function>();
    absolute->name = "__builtin::abs";
    math->methods.emplace("abs", absolute);
    state_->standard_modules.emplace(math->name, math);
}
AdvancedEngine::~AdvancedEngine() = default;
AdvancedEngine::AdvancedEngine(AdvancedEngine&&) noexcept = default;
AdvancedEngine& AdvancedEngine::operator=(AdvancedEngine&&) noexcept = default;

auto AdvancedEngine::execute(std::string_view source) -> std::expected<Value, ExecutionError> {
    try {
        auto parsed = parse_program(source);
        if (!parsed) return std::unexpected(parsed.error());
        ++state_->statistics.scripts;
        Dynamic result = execute_statements(*state_, state_->globals, parsed->statements);
        const Value public_result = to_public_value(*state_, result);
        const std::size_t collected = state_->heap.collect({state_->globals}, {result});
        ++state_->statistics.collections;
        state_->statistics.objects_collected += collected;
        return public_result;
    } catch (const RuntimeFailure& failure) {
        return std::unexpected(failure.error);
    } catch (const RaiseSignal& raised) {
        return std::unexpected(ExecutionError{"uncaught exception: " + inspect(*state_, raised.value),
                                              raised.location.line, raised.location.column});
    } catch (const ReturnSignal&) {
        return std::unexpected(ExecutionError{"return used outside a function", 1, 1});
    } catch (const std::exception& error) {
        return std::unexpected(ExecutionError{error.what(), 1, 1});
    }
}

} // namespace alk::detail
