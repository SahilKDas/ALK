#include "runtime_v2_internal.hpp"

#include <cmath>
#include <limits>
#include <map>

namespace alk::detail {
namespace {

class BytecodeCompiler {
public:
    BytecodeCompiler(std::string_view name, const std::vector<std::string>& parameters)
        : parameters_(parameters) {
        chunk_.name = std::string(name);
        chunk_.parameter_count = static_cast<std::uint32_t>(parameters.size());
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            parameter_indices_.emplace(parameters[index], static_cast<std::uint32_t>(index));
        }
    }

    std::expected<BytecodeChunk, ExecutionError> compile(const StatementList& statements) {
        try {
            std::optional<std::uint32_t> last;
            for (const auto& statement : statements) {
                switch (statement->kind) {
                case StmtKind::expression:
                    last = expression(statement->expression);
                    break;
                case StmtKind::assignment: {
                    const auto source = expression(statement->expression);
                    const auto name = name_index(statement->name);
                    emit({OpCode::store_name, source, name, 0, statement->location});
                    last = source;
                    break;
                }
                case StmtKind::return_value: {
                    const auto source = statement->expression
                        ? expression(statement->expression) : constant(Dynamic{}, statement->location);
                    emit({OpCode::return_value, source, 0, 0, statement->location});
                    returned_ = true;
                    break;
                }
                default:
                    return fallback(statements);
                }
            }
            if (!returned_) {
                const auto source = last.value_or(constant(Dynamic{}, {1, 1}));
                emit({OpCode::return_value, source, 0, 0, {1, 1}});
            }
            chunk_.register_count = next_register_;
            return std::move(chunk_);
        } catch (const ExecutionError&) {
            return fallback(statements);
        }
    }

private:
    std::expected<BytecodeChunk, ExecutionError> fallback(const StatementList& statements) {
        chunk_.constants.clear();
        chunk_.names.clear();
        chunk_.code.clear();
        chunk_.ast_fragments.clear();
        chunk_.ast_fragments.push_back(statements);
        chunk_.register_count = 1;
        chunk_.code.push_back({OpCode::execute_ast, 0, 0, 0, {1, 1}});
        chunk_.code.push_back({OpCode::return_value, 0, 0, 0, {1, 1}});
        return std::move(chunk_);
    }

    std::uint32_t allocate() { return next_register_++; }
    void emit(Instruction instruction) { chunk_.code.push_back(instruction); }
    std::uint32_t constant(Dynamic value, SourceLocation location) {
        const auto index = static_cast<std::uint32_t>(chunk_.constants.size());
        chunk_.constants.push_back(std::move(value));
        const auto target = allocate();
        emit({OpCode::load_constant, target, index, 0, location});
        return target;
    }
    std::uint32_t name_index(const std::string& name) {
        const auto found = std::find(chunk_.names.begin(), chunk_.names.end(), name);
        if (found != chunk_.names.end()) return static_cast<std::uint32_t>(found - chunk_.names.begin());
        chunk_.names.push_back(name);
        return static_cast<std::uint32_t>(chunk_.names.size() - 1);
    }
    std::uint32_t expression(const ExprPtr& value) {
        if (!value) throw ExecutionError{"missing expression", 1, 1};
        switch (value->kind) {
        case ExprKind::literal:
            if (value->text == "interpolated") {
                throw ExecutionError{"interpolation requires AST execution", value->location.line, value->location.column};
            }
            if (!value->literal.is_nil() && !value->literal.is_bool() && !value->literal.is_number() &&
                !value->literal.is_string()) {
                throw ExecutionError{"complex constant requires AST execution", value->location.line, value->location.column};
            }
            return constant(value->literal, value->location);
        case ExprKind::variable: {
            const auto target = allocate();
            if (const auto parameter = parameter_indices_.find(value->text);
                parameter != parameter_indices_.end()) {
                emit({OpCode::load_parameter, target, parameter->second, 0, value->location});
            } else {
                emit({OpCode::load_name, target, name_index(value->text), 0, value->location});
            }
            return target;
        }
        case ExprKind::unary: {
            const auto source = expression(value->right);
            const auto target = allocate();
            const auto operation = value->text == "-" ? OpCode::negate : OpCode::logical_not;
            emit({operation, target, source, 0, value->location});
            return target;
        }
        case ExprKind::binary: {
            const auto left = expression(value->left);
            const auto right = expression(value->right);
            const auto target = allocate();
            const auto operation = opcode(value->text, value->location);
            emit({operation, target, left, right, value->location});
            return target;
        }
        default:
            throw ExecutionError{"expression requires AST execution", value->location.line, value->location.column};
        }
    }
    static OpCode opcode(std::string_view operation, SourceLocation location) {
        if (operation == "+") return OpCode::add;
        if (operation == "-") return OpCode::subtract;
        if (operation == "*") return OpCode::multiply;
        if (operation == "/") return OpCode::divide;
        if (operation == "%") return OpCode::modulo;
        if (operation == "==") return OpCode::equal;
        if (operation == "!=") return OpCode::not_equal;
        if (operation == "<") return OpCode::less;
        if (operation == "<=") return OpCode::less_equal;
        if (operation == ">") return OpCode::greater;
        if (operation == ">=") return OpCode::greater_equal;
        throw ExecutionError{"operator requires AST execution", location.line, location.column};
    }

    BytecodeChunk chunk_;
    std::vector<std::string> parameters_;
    std::map<std::string, std::uint32_t, std::less<>> parameter_indices_;
    std::uint32_t next_register_{};
    bool returned_{false};
};

bool register_valid(const BytecodeChunk& chunk, std::uint32_t value) {
    return value < chunk.register_count;
}

[[noreturn]] void vm_error(const Instruction& instruction, std::string message) {
    throw RuntimeFailure{{std::move(message), instruction.location.line, instruction.location.column}};
}

Dynamic arithmetic(const Instruction& instruction, const Dynamic& left, const Dynamic& right) {
    if (instruction.op == OpCode::add && left.is_string() && right.is_string()) {
        return Dynamic(std::get<std::string>(left.storage) + std::get<std::string>(right.storage));
    }
    if (!left.is_number() || !right.is_number()) vm_error(instruction, "bytecode arithmetic requires numbers");
    if ((instruction.op == OpCode::divide || instruction.op == OpCode::modulo) && right.number() == 0.0) {
        vm_error(instruction, "division by zero");
    }
    if (left.is_int() && right.is_int() && instruction.op != OpCode::divide) {
        const auto lhs = left.integer();
        const auto rhs = right.integer();
        const auto checked = [&](long double result) {
            if (result < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
                result > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
                vm_error(instruction, "integer overflow");
            }
            return Dynamic(static_cast<std::int64_t>(result));
        };
        if (instruction.op == OpCode::add) return checked(static_cast<long double>(lhs) + rhs);
        if (instruction.op == OpCode::subtract) return checked(static_cast<long double>(lhs) - rhs);
        if (instruction.op == OpCode::multiply) return checked(static_cast<long double>(lhs) * rhs);
        if (instruction.op == OpCode::modulo) {
            if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1) return Dynamic(std::int64_t{0});
            return Dynamic(lhs % rhs);
        }
    }
    const double lhs = left.number();
    const double rhs = right.number();
    if (instruction.op == OpCode::add) return Dynamic(lhs + rhs);
    if (instruction.op == OpCode::subtract) return Dynamic(lhs - rhs);
    if (instruction.op == OpCode::multiply) return Dynamic(lhs * rhs);
    if (instruction.op == OpCode::divide) return Dynamic(lhs / rhs);
    return Dynamic(std::fmod(lhs, rhs));
}

} // namespace

auto compile_function_bytecode(std::string_view name,
                               const std::vector<std::string>& parameters,
                               const StatementList& body)
    -> std::expected<BytecodeChunk, ExecutionError> {
    return BytecodeCompiler(name, parameters).compile(body);
}

auto verify_bytecode(const BytecodeChunk& chunk) -> std::expected<void, ExecutionError> {
    if (chunk.register_count == 0 || chunk.register_count > 65536U) {
        return std::unexpected(ExecutionError{"invalid bytecode register count", 1, 1});
    }
    if (chunk.code.empty() || chunk.code.size() > 1'000'000U) {
        return std::unexpected(ExecutionError{"invalid bytecode instruction count", 1, 1});
    }
    bool has_return = false;
    for (const auto& instruction : chunk.code) {
        const auto bad = [&](std::string message) {
            return std::unexpected(ExecutionError{std::move(message), instruction.location.line,
                                                  instruction.location.column});
        };
        switch (instruction.op) {
        case OpCode::load_constant:
            if (!register_valid(chunk, instruction.a) || instruction.b >= chunk.constants.size()) {
                return bad("invalid constant bytecode operands");
            }
            break;
        case OpCode::load_parameter:
            if (!register_valid(chunk, instruction.a) || instruction.b >= chunk.parameter_count) {
                return bad("invalid parameter bytecode operands");
            }
            break;
        case OpCode::load_name:
            if (!register_valid(chunk, instruction.a) || instruction.b >= chunk.names.size()) {
                return bad("invalid name bytecode operands");
            }
            break;
        case OpCode::store_name:
            if (!register_valid(chunk, instruction.a) || instruction.b >= chunk.names.size()) {
                return bad("invalid store bytecode operands");
            }
            break;
        case OpCode::move:
        case OpCode::negate:
        case OpCode::logical_not:
            if (!register_valid(chunk, instruction.a) || !register_valid(chunk, instruction.b)) {
                return bad("invalid unary bytecode operands");
            }
            break;
        case OpCode::add:
        case OpCode::subtract:
        case OpCode::multiply:
        case OpCode::divide:
        case OpCode::modulo:
        case OpCode::equal:
        case OpCode::not_equal:
        case OpCode::less:
        case OpCode::less_equal:
        case OpCode::greater:
        case OpCode::greater_equal:
            if (!register_valid(chunk, instruction.a) || !register_valid(chunk, instruction.b) ||
                !register_valid(chunk, instruction.c)) {
                return bad("invalid binary bytecode operands");
            }
            break;
        case OpCode::jump:
            if (instruction.a >= chunk.code.size()) return bad("jump target is outside bytecode");
            break;
        case OpCode::jump_if_false:
            if (!register_valid(chunk, instruction.a) || instruction.b >= chunk.code.size()) {
                return bad("invalid conditional jump operands");
            }
            break;
        case OpCode::execute_ast:
            if (!register_valid(chunk, instruction.a) || instruction.b >= chunk.ast_fragments.size()) {
                return bad("invalid AST fallback bytecode operands");
            }
            break;
        case OpCode::return_value:
            if (!register_valid(chunk, instruction.a)) return bad("invalid return register");
            has_return = true;
            break;
        }
    }
    if (!has_return) return std::unexpected(ExecutionError{"bytecode has no return", 1, 1});
    return {};
}

Dynamic execute_bytecode(RuntimeState& state, const BytecodeChunk& chunk,
                         const std::shared_ptr<Scope>& scope,
                         const std::vector<Dynamic>& arguments) {
    auto verified = verify_bytecode(chunk);
    ++state.statistics.verifier_runs;
    if (!verified) throw RuntimeFailure{verified.error()};
    if (arguments.size() != chunk.parameter_count) {
        throw RuntimeFailure{{"bytecode argument count mismatch", 1, 1}};
    }
    std::vector<Dynamic> registers(chunk.register_count);
    std::size_t ip = 0;
    while (ip < chunk.code.size()) {
        const Instruction& instruction = chunk.code[ip++];
        ++state.statistics.bytecode_instructions;
        switch (instruction.op) {
        case OpCode::load_constant: registers[instruction.a] = chunk.constants[instruction.b]; break;
        case OpCode::load_parameter: registers[instruction.a] = arguments[instruction.b]; break;
        case OpCode::load_name: {
            const auto value = scope->get(chunk.names[instruction.b]);
            if (!value) vm_error(instruction, "undefined variable '" + chunk.names[instruction.b] + "'");
            registers[instruction.a] = *value;
            break;
        }
        case OpCode::store_name:
            scope->define(chunk.names[instruction.b], registers[instruction.a]);
            break;
        case OpCode::move: registers[instruction.a] = registers[instruction.b]; break;
        case OpCode::add:
        case OpCode::subtract:
        case OpCode::multiply:
        case OpCode::divide:
        case OpCode::modulo:
            registers[instruction.a] = arithmetic(instruction, registers[instruction.b], registers[instruction.c]);
            break;
        case OpCode::negate:
            if (!registers[instruction.b].is_number()) vm_error(instruction, "negate requires a number");
            registers[instruction.a] = registers[instruction.b].is_int()
                ? Dynamic(-registers[instruction.b].integer()) : Dynamic(-registers[instruction.b].number());
            break;
        case OpCode::logical_not:
            registers[instruction.a] = Dynamic(!registers[instruction.b].truthy());
            break;
        case OpCode::equal:
        case OpCode::not_equal: {
            const bool equal = dynamic_equal(state, registers[instruction.b], registers[instruction.c]);
            registers[instruction.a] = Dynamic(instruction.op == OpCode::equal ? equal : !equal);
            break;
        }
        case OpCode::less:
        case OpCode::less_equal:
        case OpCode::greater:
        case OpCode::greater_equal: {
            const Dynamic& left = registers[instruction.b];
            const Dynamic& right = registers[instruction.c];
            if (!left.is_number() || !right.is_number()) vm_error(instruction, "comparison requires numbers");
            bool result = false;
            if (instruction.op == OpCode::less) result = left.number() < right.number();
            if (instruction.op == OpCode::less_equal) result = left.number() <= right.number();
            if (instruction.op == OpCode::greater) result = left.number() > right.number();
            if (instruction.op == OpCode::greater_equal) result = left.number() >= right.number();
            registers[instruction.a] = Dynamic(result);
            break;
        }
        case OpCode::jump: ip = instruction.a; break;
        case OpCode::jump_if_false:
            if (!registers[instruction.a].truthy()) ip = instruction.b;
            break;
        case OpCode::execute_ast:
            registers[instruction.a] = execute_statements(
                state, scope, chunk.ast_fragments[instruction.b]);
            break;
        case OpCode::return_value: return registers[instruction.a];
        }
    }
    throw RuntimeFailure{{"bytecode terminated without return", 1, 1}};
}

} // namespace alk::detail
