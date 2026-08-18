#pragma once

#include <alk/alk.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace alk::detail {

struct SourceLocation {
    std::size_t line{1};
    std::size_t column{1};
};

struct Expr;
struct Stmt;
struct Scope;
struct Function;
struct Class;
struct Module;
struct BlockValue;
struct DynamicArray;
struct DynamicMap;
struct RangeValue;
struct NativeCode;

using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;
using StatementList = std::vector<StmtPtr>;

struct ObjectRef {
    std::size_t id{};
    friend bool operator==(ObjectRef, ObjectRef) = default;
};

struct AlkNamespace {
    friend bool operator==(AlkNamespace, AlkNamespace) = default;
};

class Dynamic {
public:
    using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string,
                                 std::shared_ptr<DynamicArray>, std::shared_ptr<DynamicMap>,
                                 ObjectRef, std::shared_ptr<Function>, std::shared_ptr<Class>,
                                 std::shared_ptr<Module>, std::shared_ptr<BlockValue>,
                                 std::shared_ptr<RangeValue>, AlkNamespace>;

    Dynamic() = default;
    Dynamic(std::nullptr_t) {}
    Dynamic(bool value) : storage(value) {}
    Dynamic(std::int64_t value) : storage(value) {}
    Dynamic(double value) : storage(value) {}
    Dynamic(std::string value) : storage(std::move(value)) {}
    Dynamic(std::string_view value) : storage(std::string(value)) {}
    Dynamic(const char* value) : storage(std::string(value)) {}
    Dynamic(std::shared_ptr<DynamicArray> value) : storage(std::move(value)) {}
    Dynamic(std::shared_ptr<DynamicMap> value) : storage(std::move(value)) {}
    Dynamic(ObjectRef value) : storage(value) {}
    Dynamic(std::shared_ptr<Function> value) : storage(std::move(value)) {}
    Dynamic(std::shared_ptr<Class> value) : storage(std::move(value)) {}
    Dynamic(std::shared_ptr<Module> value) : storage(std::move(value)) {}
    Dynamic(std::shared_ptr<BlockValue> value) : storage(std::move(value)) {}
    Dynamic(std::shared_ptr<RangeValue> value) : storage(std::move(value)) {}
    Dynamic(AlkNamespace value) : storage(value) {}

    [[nodiscard]] bool is_nil() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_int() const noexcept;
    [[nodiscard]] bool is_float() const noexcept;
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_map() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;
    [[nodiscard]] bool is_function() const noexcept;
    [[nodiscard]] bool is_class() const noexcept;
    [[nodiscard]] bool is_module() const noexcept;
    [[nodiscard]] bool is_block() const noexcept;
    [[nodiscard]] bool is_range() const noexcept;
    [[nodiscard]] bool is_alk_namespace() const noexcept;
    [[nodiscard]] bool truthy() const noexcept;
    [[nodiscard]] double number() const;
    [[nodiscard]] std::int64_t integer() const;
    [[nodiscard]] std::string type_name() const;

    Storage storage;
};

struct DynamicArray { std::vector<Dynamic> values; };
struct DynamicMap { std::map<std::string, Dynamic, std::less<>> values; };
struct RangeValue {
    std::int64_t first{};
    std::optional<std::int64_t> last;
    bool exclusive{false};
};

enum class ExprKind {
    literal,
    variable,
    array,
    map,
    unary,
    binary,
    index,
    property,
    safe_property,
    call,
    range,
    comprehension
};

struct Expr {
    ExprKind kind{ExprKind::literal};
    SourceLocation location;
    Dynamic literal;
    std::string text;
    ExprPtr left;
    ExprPtr right;
    ExprPtr third;
    std::vector<ExprPtr> items;
    std::vector<std::pair<std::string, ExprPtr>> entries;
    std::vector<ExprPtr> arguments;
    std::vector<std::string> block_parameters;
    StatementList block_body;
};

enum class StmtKind {
    expression,
    assignment,
    property_assignment,
    index_assignment,
    print,
    return_value,
    function_definition,
    class_definition,
    module_definition,
    include_module,
    prepend_module,
    raise_value,
    rescue_block,
    import_names,
    conditional,
    while_loop
};

struct Stmt {
    StmtKind kind{StmtKind::expression};
    SourceLocation location;
    std::string name;
    std::string secondary_name;
    std::vector<std::string> names;
    ExprPtr expression;
    ExprPtr receiver;
    ExprPtr index;
    StatementList body;
    StatementList rescue_body;
};

struct ParseResult { StatementList statements; };

[[nodiscard]] auto parse_program(std::string_view source)
    -> std::expected<ParseResult, ExecutionError>;

struct Scope : std::enable_shared_from_this<Scope> {
    std::map<std::string, Dynamic, std::less<>> values;
    std::shared_ptr<Scope> parent;

    [[nodiscard]] std::optional<Dynamic> get(std::string_view name) const;
    void define(std::string name, Dynamic value);
};

enum class OpCode : std::uint8_t {
    load_constant,
    load_parameter,
    load_name,
    store_name,
    move,
    add,
    subtract,
    multiply,
    divide,
    modulo,
    negate,
    logical_not,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    jump,
    jump_if_false,
    execute_ast,
    return_value
};

struct Instruction {
    OpCode op{OpCode::load_constant};
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t c{};
    SourceLocation location;
};

struct BytecodeChunk {
    std::string name;
    std::uint32_t register_count{};
    std::uint32_t parameter_count{};
    std::vector<Dynamic> constants;
    std::vector<std::string> names;
    std::vector<Instruction> code;
    std::vector<StatementList> ast_fragments;
};

[[nodiscard]] auto compile_function_bytecode(
    std::string_view name,
    const std::vector<std::string>& parameters,
    const StatementList& body)
    -> std::expected<BytecodeChunk, ExecutionError>;

[[nodiscard]] auto verify_bytecode(const BytecodeChunk& chunk)
    -> std::expected<void, ExecutionError>;

struct Function {
    std::string name;
    std::vector<std::string> parameters;
    StatementList body;
    std::shared_ptr<Scope> closure;
    bool method{false};
    std::weak_ptr<Class> owner_class;
    std::optional<Dynamic> bound_receiver;
    std::optional<BytecodeChunk> bytecode;
    std::shared_ptr<NativeCode> native_code;
};

struct BlockValue {
    std::vector<std::string> parameters;
    StatementList body;
    std::shared_ptr<Scope> closure;
};

struct Module {
    std::string name;
    std::map<std::string, std::shared_ptr<Function>, std::less<>> methods;
};

struct Class {
    std::string name;
    std::shared_ptr<Class> parent;
    std::map<std::string, std::shared_ptr<Function>, std::less<>> methods;
    std::vector<std::shared_ptr<Module>> included;
    std::vector<std::shared_ptr<Module>> prepended;
};

struct Object {
    std::shared_ptr<Class> klass;
    std::map<std::string, Dynamic, std::less<>> fields;
    bool marked{false};
};

class ManagedHeap {
public:
    [[nodiscard]] ObjectRef allocate(std::shared_ptr<Class> klass);
    [[nodiscard]] Object& get(ObjectRef ref);
    [[nodiscard]] const Object& get(ObjectRef ref) const;
    [[nodiscard]] std::size_t live_objects() const noexcept;
    std::size_t collect(const std::vector<std::shared_ptr<Scope>>& roots,
                        const std::vector<Dynamic>& value_roots);

private:
    void mark_dynamic(const Dynamic& value, std::set<const Scope*>& visited_scopes);
    void mark_scope(const std::shared_ptr<Scope>& scope, std::set<const Scope*>& visited_scopes);
    std::vector<std::unique_ptr<Object>> objects_;
    std::vector<std::size_t> free_ids_;
};

struct RuntimeStatistics {
    std::uint64_t scripts{};
    std::uint64_t function_calls{};
    std::uint64_t bytecode_functions{};
    std::uint64_t bytecode_instructions{};
    std::uint64_t verifier_runs{};
    std::uint64_t jit_compilations{};
    std::uint64_t native_calls{};
    std::uint64_t jit_fallbacks{};
    std::uint64_t collections{};
    std::uint64_t objects_collected{};
};

struct RuntimeState {
    std::shared_ptr<Scope> globals{std::make_shared<Scope>()};
    ManagedHeap heap;
    RuntimeStatistics statistics;
    std::vector<std::shared_ptr<Scope>> active_scopes;
    std::map<std::string, std::shared_ptr<Module>, std::less<>> standard_modules;
};

struct ReturnSignal { Dynamic value; };
struct RaiseSignal { Dynamic value; SourceLocation location; };
struct RuntimeFailure { ExecutionError error; };

[[nodiscard]] Dynamic evaluate_expression(RuntimeState& state,
                                          const std::shared_ptr<Scope>& scope,
                                          const ExprPtr& expression,
                                          const std::shared_ptr<BlockValue>& block = {});
[[nodiscard]] Dynamic execute_statements(RuntimeState& state,
                                         const std::shared_ptr<Scope>& scope,
                                         const StatementList& statements,
                                         const std::shared_ptr<BlockValue>& block = {});

[[nodiscard]] Dynamic execute_bytecode(RuntimeState& state,
                                       const BytecodeChunk& chunk,
                                       const std::shared_ptr<Scope>& scope,
                                       const std::vector<Dynamic>& arguments);

[[nodiscard]] std::shared_ptr<NativeCode> compile_native(const BytecodeChunk& chunk);
[[nodiscard]] bool native_available() noexcept;
[[nodiscard]] std::optional<std::int64_t> invoke_native(
    const std::shared_ptr<NativeCode>& code,
    const std::vector<Dynamic>& arguments);

[[nodiscard]] Value to_public_value(RuntimeState& state, const Dynamic& value);
[[nodiscard]] Dynamic from_public_value(const Value& value);
[[nodiscard]] std::string inspect(RuntimeState& state, const Dynamic& value);
[[nodiscard]] bool dynamic_equal(RuntimeState& state, const Dynamic& left, const Dynamic& right);

class AdvancedEngine {
public:
    AdvancedEngine();
    ~AdvancedEngine();
    AdvancedEngine(AdvancedEngine&&) noexcept;
    AdvancedEngine& operator=(AdvancedEngine&&) noexcept;
    AdvancedEngine(const AdvancedEngine&) = delete;
    AdvancedEngine& operator=(const AdvancedEngine&) = delete;

    [[nodiscard]] auto execute(std::string_view source)
        -> std::expected<Value, ExecutionError>;

private:
    std::unique_ptr<RuntimeState> state_;
};

} // namespace alk::detail
