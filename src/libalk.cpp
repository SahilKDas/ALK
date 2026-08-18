#include <alk/alk.hpp>
#include "runtime_v2_internal.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace alk {
namespace {

[[nodiscard]] std::string escape_json(std::string_view input) {
    std::string output;
    output.reserve(input.size() + 2);
    output.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : input) {
        switch (ch) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (ch < 0x20U) {
                output += "\\u00";
                output.push_back(hex[(ch >> 4U) & 0x0fU]);
                output.push_back(hex[ch & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(ch));
            }
        }
    }
    output.push_back('"');
    return output;
}

[[nodiscard]] std::string number_to_string(double value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("non-finite numbers cannot be serialized as JSON");
    }
    char buffer[128]{};
    const auto [end, error] = std::to_chars(
        std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (error != std::errc{}) {
        throw std::runtime_error("failed to serialize floating-point number");
    }
    return std::string(buffer, end);
}

[[nodiscard]] std::string display(const Value& value) {
    if (value.is_string()) {
        return value.as_string();
    }
    return value.inspect();
}

struct Failure final : std::exception {
    explicit Failure(ExecutionError value) : error(std::move(value)) {}
    ExecutionError error;
};

enum class TokenKind {
    eof,
    newline,
    identifier,
    number,
    string,
    left_paren,
    right_paren,
    left_bracket,
    right_bracket,
    left_brace,
    right_brace,
    comma,
    colon,
    dot,
    plus,
    minus,
    star,
    slash,
    percent,
    bang,
    equal,
    equal_equal,
    bang_equal,
    less,
    less_equal,
    greater,
    greater_equal
};

struct Token {
    TokenKind kind{TokenKind::eof};
    std::string text;
    std::size_t line{1};
    std::size_t column{1};
    bool interpolated{false};
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    [[nodiscard]] std::vector<Token> scan() {
        std::vector<Token> tokens;
        while (!at_end()) {
            const auto line = line_;
            const auto column = column_;
            const char ch = peek();
            if (ch == ' ' || ch == '\t' || ch == '\r') {
                advance();
                continue;
            }
            if (ch == '\n') {
                advance();
                tokens.push_back({TokenKind::newline, "\n", line, column});
                continue;
            }
            if (ch == '#') {
                while (!at_end() && peek() != '\n') {
                    advance();
                }
                continue;
            }
            if (is_identifier_start(ch)) {
                tokens.push_back(identifier());
                continue;
            }
            if (ch >= '0' && ch <= '9') {
                tokens.push_back(number());
                continue;
            }
            if (ch == '"' || ch == '\'') {
                tokens.push_back(string());
                continue;
            }

            advance();
            switch (ch) {
            case '(': tokens.push_back({TokenKind::left_paren, "(", line, column}); break;
            case ')': tokens.push_back({TokenKind::right_paren, ")", line, column}); break;
            case '[': tokens.push_back({TokenKind::left_bracket, "[", line, column}); break;
            case ']': tokens.push_back({TokenKind::right_bracket, "]", line, column}); break;
            case '{': tokens.push_back({TokenKind::left_brace, "{", line, column}); break;
            case '}': tokens.push_back({TokenKind::right_brace, "}", line, column}); break;
            case ',': tokens.push_back({TokenKind::comma, ",", line, column}); break;
            case ':': tokens.push_back({TokenKind::colon, ":", line, column}); break;
            case '.': tokens.push_back({TokenKind::dot, ".", line, column}); break;
            case '+': tokens.push_back({TokenKind::plus, "+", line, column}); break;
            case '-': tokens.push_back({TokenKind::minus, "-", line, column}); break;
            case '*': tokens.push_back({TokenKind::star, "*", line, column}); break;
            case '/': tokens.push_back({TokenKind::slash, "/", line, column}); break;
            case '%': tokens.push_back({TokenKind::percent, "%", line, column}); break;
            case '!':
                tokens.push_back(match('=')
                    ? Token{TokenKind::bang_equal, "!=", line, column}
                    : Token{TokenKind::bang, "!", line, column});
                break;
            case '=':
                tokens.push_back(match('=')
                    ? Token{TokenKind::equal_equal, "==", line, column}
                    : Token{TokenKind::equal, "=", line, column});
                break;
            case '<':
                tokens.push_back(match('=')
                    ? Token{TokenKind::less_equal, "<=", line, column}
                    : Token{TokenKind::less, "<", line, column});
                break;
            case '>':
                tokens.push_back(match('=')
                    ? Token{TokenKind::greater_equal, ">=", line, column}
                    : Token{TokenKind::greater, ">", line, column});
                break;
            default:
                fail("unexpected character", line, column);
            }
        }
        tokens.push_back({TokenKind::eof, "", line_, column_});
        return tokens;
    }

private:
    [[nodiscard]] bool at_end() const noexcept { return position_ >= source_.size(); }
    [[nodiscard]] char peek(std::size_t offset = 0) const noexcept {
        const auto index = position_ + offset;
        return index < source_.size() ? source_[index] : '\0';
    }
    char advance() noexcept {
        const char ch = source_[position_++];
        if (ch == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return ch;
    }
    bool match(char expected) noexcept {
        if (peek() != expected) return false;
        advance();
        return true;
    }
    [[noreturn]] static void fail(std::string message, std::size_t line, std::size_t column) {
        throw Failure({std::move(message), line, column});
    }
    static bool is_identifier_start(char ch) noexcept {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    }
    static bool is_identifier_continue(char ch) noexcept {
        return is_identifier_start(ch) || (ch >= '0' && ch <= '9');
    }

    static int hex_value(char ch) noexcept {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    std::uint32_t unicode_escape() {
        std::uint32_t codepoint = 0;
        for (int i = 0; i < 4; ++i) {
            if (at_end()) fail("incomplete Unicode escape", line_, column_);
            const int digit = hex_value(advance());
            if (digit < 0) fail("invalid Unicode escape", line_, column_ - 1);
            codepoint = (codepoint << 4U) | static_cast<std::uint32_t>(digit);
        }
        return codepoint;
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    Token identifier() {
        const auto start = position_;
        const auto line = line_;
        const auto column = column_;
        advance();
        while (is_identifier_continue(peek())) advance();
        if (peek() == '?' || peek() == '!') advance();
        return {TokenKind::identifier, std::string(source_.substr(start, position_ - start)), line, column};
    }

    Token number() {
        const auto start = position_;
        const auto line = line_;
        const auto column = column_;
        while (peek() >= '0' && peek() <= '9') advance();
        if (peek() == '.' && peek(1) >= '0' && peek(1) <= '9') {
            advance();
            while (peek() >= '0' && peek() <= '9') advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            advance();
            if (peek() == '+' || peek() == '-') advance();
            if (!(peek() >= '0' && peek() <= '9')) fail("expected exponent digits", line_, column_);
            while (peek() >= '0' && peek() <= '9') advance();
        }
        return {TokenKind::number, std::string(source_.substr(start, position_ - start)), line, column};
    }

    Token string() {
        const auto line = line_;
        const auto column = column_;
        const char quote = advance();
        std::string value;
        bool interpolated = false;
        while (!at_end() && peek() != quote) {
            if (peek() == '\n') fail("unterminated string", line, column);
            char ch = advance();
            if (ch == '#' && peek() == '{' && quote == '"') {
                interpolated = true;
                value += "#{";
                advance();
                continue;
            }
            if (ch != '\\') {
                value.push_back(ch);
                continue;
            }
            if (at_end()) fail("unterminated escape sequence", line_, column_);
            ch = advance();
            switch (ch) {
            case '"': value.push_back('"'); break;
            case '\'': value.push_back('\''); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint = unicode_escape();
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (!match('\\') || !match('u')) {
                        fail("missing low Unicode surrogate", line_, column_);
                    }
                    const std::uint32_t low = unicode_escape();
                    if (low < 0xdc00U || low > 0xdfffU) {
                        fail("invalid low Unicode surrogate", line_, column_);
                    }
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    fail("unexpected low Unicode surrogate", line_, column_);
                }
                append_utf8(value, codepoint);
                break;
            }
            default: fail("unsupported string escape", line_, column_ - 1);
            }
        }
        if (at_end()) fail("unterminated string", line, column);
        advance();
        return {TokenKind::string, std::move(value), line, column, interpolated};
    }

    std::string_view source_;
    std::size_t position_{0};
    std::size_t line_{1};
    std::size_t column_{1};
};

using Environment = std::map<std::string, Value, std::less<>>;

class Engine {
public:
    Engine(std::string_view source, Environment& environment)
        : tokens_(Lexer(source).scan()), environment_(environment) {}

    [[nodiscard]] Value run() {
        Value result;
        skip_newlines();
        while (!check(TokenKind::eof)) {
            result = statement();
            if (!check(TokenKind::newline) && !check(TokenKind::eof)) {
                fail(current(), "expected a new line after expression");
            }
            skip_newlines();
        }
        return result;
    }

    [[nodiscard]] Value expression_only() {
        skip_newlines();
        Value result = expression();
        skip_newlines();
        if (!check(TokenKind::eof)) fail(current(), "unexpected token after expression");
        return result;
    }

private:
    [[nodiscard]] const Token& current() const noexcept { return tokens_[position_]; }
    [[nodiscard]] const Token& previous() const noexcept { return tokens_[position_ - 1]; }
    [[nodiscard]] bool check(TokenKind kind) const noexcept { return current().kind == kind; }
    [[nodiscard]] bool check_identifier(std::string_view text) const noexcept {
        return check(TokenKind::identifier) && current().text == text;
    }
    const Token& advance() noexcept {
        if (!check(TokenKind::eof)) ++position_;
        return previous();
    }
    bool match(TokenKind kind) noexcept {
        if (!check(kind)) return false;
        advance();
        return true;
    }
    bool match_identifier(std::string_view text) noexcept {
        if (!check_identifier(text)) return false;
        advance();
        return true;
    }
    const Token& consume(TokenKind kind, std::string message) {
        if (check(kind)) return advance();
        fail(current(), std::move(message));
    }
    [[noreturn]] static void fail(const Token& token, std::string message) {
        throw Failure({std::move(message), token.line, token.column});
    }
    void skip_newlines() noexcept {
        while (match(TokenKind::newline)) {}
    }

    Value statement() {
        if (match_identifier("puts")) {
            Value value = expression();
            std::cout << display(value) << '\n';
            return value;
        }
        if (check(TokenKind::identifier) && tokens_[position_ + 1].kind == TokenKind::equal) {
            const Token name = advance();
            advance();
            Value value = expression();
            environment_.insert_or_assign(name.text, value);
            return value;
        }
        return expression();
    }

    Value expression() { return logical_or(); }

    Value logical_or() {
        Value left = logical_and();
        while (match_identifier("or")) {
            Value right = logical_and();
            left = Value(left.truthy() || right.truthy());
        }
        return left;
    }

    Value logical_and() {
        Value left = equality();
        while (match_identifier("and")) {
            Value right = equality();
            left = Value(left.truthy() && right.truthy());
        }
        return left;
    }

    Value equality() {
        Value left = comparison();
        while (check(TokenKind::equal_equal) || check(TokenKind::bang_equal)) {
            const auto operation = advance().kind;
            Value right = comparison();
            const bool equal = left == right;
            left = Value(operation == TokenKind::equal_equal ? equal : !equal);
        }
        return left;
    }

    Value comparison() {
        Value left = term();
        while (check(TokenKind::less) || check(TokenKind::less_equal) ||
               check(TokenKind::greater) || check(TokenKind::greater_equal)) {
            const Token operation = advance();
            Value right = term();
            bool result = false;
            if (left.is_number() && right.is_number()) {
                const double lhs = left.as_float();
                const double rhs = right.as_float();
                if (operation.kind == TokenKind::less) result = lhs < rhs;
                if (operation.kind == TokenKind::less_equal) result = lhs <= rhs;
                if (operation.kind == TokenKind::greater) result = lhs > rhs;
                if (operation.kind == TokenKind::greater_equal) result = lhs >= rhs;
            } else if (left.is_string() && right.is_string()) {
                if (operation.kind == TokenKind::less) result = left.as_string() < right.as_string();
                if (operation.kind == TokenKind::less_equal) result = left.as_string() <= right.as_string();
                if (operation.kind == TokenKind::greater) result = left.as_string() > right.as_string();
                if (operation.kind == TokenKind::greater_equal) result = left.as_string() >= right.as_string();
            } else {
                fail(operation, "comparison requires two numbers or two strings");
            }
            left = Value(result);
        }
        return left;
    }

    Value term() {
        Value left = factor();
        while (check(TokenKind::plus) || check(TokenKind::minus)) {
            const Token operation = advance();
            Value right = factor();
            if (operation.kind == TokenKind::plus && left.is_string() && right.is_string()) {
                left = Value(left.as_string() + right.as_string());
                continue;
            }
            require_numbers(operation, left, right);
            if (left.is_int() && right.is_int()) {
                left = Value(operation.kind == TokenKind::plus
                    ? left.as_int() + right.as_int()
                    : left.as_int() - right.as_int());
            } else {
                left = Value(operation.kind == TokenKind::plus
                    ? left.as_float() + right.as_float()
                    : left.as_float() - right.as_float());
            }
        }
        return left;
    }

    Value factor() {
        Value left = unary();
        while (check(TokenKind::star) || check(TokenKind::slash) || check(TokenKind::percent)) {
            const Token operation = advance();
            Value right = unary();
            require_numbers(operation, left, right);
            if ((operation.kind == TokenKind::slash || operation.kind == TokenKind::percent) &&
                right.as_float() == 0.0) {
                fail(operation, "division by zero");
            }
            if (operation.kind == TokenKind::star) {
                left = left.is_int() && right.is_int()
                    ? Value(left.as_int() * right.as_int())
                    : Value(left.as_float() * right.as_float());
            } else if (operation.kind == TokenKind::slash) {
                left = Value(left.as_float() / right.as_float());
            } else if (left.is_int() && right.is_int()) {
                left = Value(left.as_int() % right.as_int());
            } else {
                left = Value(std::fmod(left.as_float(), right.as_float()));
            }
        }
        return left;
    }

    Value unary() {
        if (match(TokenKind::minus)) {
            const Token operation = previous();
            Value value = unary();
            if (!value.is_number()) fail(operation, "unary minus requires a number");
            return value.is_int() ? Value(-value.as_int()) : Value(-value.as_float());
        }
        if (match(TokenKind::bang) || match_identifier("not")) {
            return Value(!unary().truthy());
        }
        return postfix();
    }

    Value postfix() {
        Value value = primary();
        while (true) {
            if (match(TokenKind::left_bracket)) {
                const Token bracket = previous();
                Value index = expression();
                consume(TokenKind::right_bracket, "expected ']' after index");
                value = index_value(bracket, value, index);
                continue;
            }
            if (match(TokenKind::dot)) {
                const Token method = consume(TokenKind::identifier, "expected method name after '.'");
                std::vector<Value> arguments;
                if (match(TokenKind::left_paren)) {
                    arguments = arguments_until_right_paren();
                }
                value = call_method(method, value, arguments);
                continue;
            }
            break;
        }
        return value;
    }

    Value primary() {
        if (match_identifier("nil") || match_identifier("null")) return Value{};
        if (match_identifier("true")) return Value(true);
        if (match_identifier("false")) return Value(false);

        if (match(TokenKind::number)) {
            const Token& token = previous();
            if (token.text.find_first_of(".eE") == std::string::npos) {
                std::int64_t result{};
                const auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(), result);
                if (parsed.ec == std::errc{}) return Value(result);
            }
            double result{};
            const auto parsed = std::from_chars(token.text.data(), token.text.data() + token.text.size(), result);
            if (parsed.ec != std::errc{}) fail(token, "invalid number");
            return Value(result);
        }
        if (match(TokenKind::string)) {
            const Token token = previous();
            return token.interpolated ? Value(interpolate(token)) : Value(token.text);
        }
        if (match(TokenKind::left_paren)) {
            Value result = expression();
            consume(TokenKind::right_paren, "expected ')' after expression");
            return result;
        }
        if (match(TokenKind::left_bracket)) return array_literal();
        if (match(TokenKind::left_brace)) return map_literal();
        if (match(TokenKind::identifier)) {
            const Token name = previous();
            if (match(TokenKind::left_paren)) {
                return call_function(name, arguments_until_right_paren());
            }
            const auto found = environment_.find(name.text);
            if (found == environment_.end()) fail(name, "undefined variable '" + name.text + "'");
            return found->second;
        }
        fail(current(), "expected expression");
    }

    Value array_literal() {
        Value::Array values;
        skip_newlines();
        if (!check(TokenKind::right_bracket)) {
            do {
                skip_newlines();
                values.push_back(expression());
                skip_newlines();
            } while (match(TokenKind::comma) && !check(TokenKind::right_bracket));
        }
        consume(TokenKind::right_bracket, "expected ']' after array");
        return Value(std::move(values));
    }

    Value map_literal() {
        Value::Map values;
        skip_newlines();
        if (!check(TokenKind::right_brace)) {
            do {
                skip_newlines();
                const Token key = consume(TokenKind::string, "map keys must be strings");
                consume(TokenKind::colon, "expected ':' after map key");
                skip_newlines();
                values.insert_or_assign(key.text, expression());
                skip_newlines();
            } while (match(TokenKind::comma) && !check(TokenKind::right_brace));
        }
        consume(TokenKind::right_brace, "expected '}' after map");
        return Value(std::move(values));
    }

    std::vector<Value> arguments_until_right_paren() {
        std::vector<Value> values;
        skip_newlines();
        if (!check(TokenKind::right_paren)) {
            do {
                skip_newlines();
                values.push_back(expression());
                skip_newlines();
            } while (match(TokenKind::comma));
        }
        consume(TokenKind::right_paren, "expected ')' after arguments");
        return values;
    }

    static void require_numbers(const Token& token, const Value& left, const Value& right) {
        if (!left.is_number() || !right.is_number()) {
            fail(token, "arithmetic requires numeric operands");
        }
    }

    static Value index_value(const Token& token, Value& value, const Value& index) {
        if (value.is_array()) {
            if (!index.is_int()) fail(token, "array index must be an integer");
            const auto raw = index.as_int();
            if (raw < 0 || static_cast<std::uint64_t>(raw) >= value.as_array().size()) {
                fail(token, "array index out of bounds");
            }
            return value.as_array()[static_cast<std::size_t>(raw)];
        }
        if (value.is_map()) {
            if (!index.is_string()) fail(token, "map index must be a string");
            const auto found = value.as_map().find(index.as_string());
            return found == value.as_map().end() ? Value{} : found->second;
        }
        if (value.is_string()) {
            if (!index.is_int()) fail(token, "string index must be an integer");
            const auto raw = index.as_int();
            if (raw < 0 || static_cast<std::uint64_t>(raw) >= value.as_string().size()) {
                fail(token, "string index out of bounds");
            }
            return Value(std::string(1, value.as_string()[static_cast<std::size_t>(raw)]));
        }
        fail(token, "value is not indexable");
    }

    Value call_function(const Token& function, const std::vector<Value>& arguments) {
        if (function.text == "parse_json") {
            require_arity(function, arguments, 1);
            if (!arguments[0].is_string()) fail(function, "parse_json expects a string");
            auto result = alk::parse_json(arguments[0].as_string());
            if (!result) throw Failure(result.error());
            return std::move(*result);
        }
        if (function.text == "puts") {
            require_arity(function, arguments, 1);
            std::cout << display(arguments[0]) << '\n';
            return arguments[0];
        }
        fail(function, "unknown function '" + function.text + "'");
    }

    Value call_method(const Token& method, Value receiver, const std::vector<Value>& arguments) {
        if (method.text == "to_json") {
            require_arity(method, arguments, 0);
            try {
                return Value(receiver.to_json());
            } catch (const std::runtime_error& error) {
                fail(method, error.what());
            }
        }
        if (method.text == "empty?") {
            require_arity(method, arguments, 0);
            if (receiver.is_string()) return Value(receiver.as_string().empty());
            if (receiver.is_array()) return Value(receiver.as_array().empty());
            if (receiver.is_map()) return Value(receiver.as_map().empty());
            fail(method, "empty? expects a string, array, or map");
        }
        if (method.text == "length" || method.text == "size") {
            require_arity(method, arguments, 0);
            if (receiver.is_string()) return Value(static_cast<std::int64_t>(receiver.as_string().size()));
            if (receiver.is_array()) return Value(static_cast<std::int64_t>(receiver.as_array().size()));
            if (receiver.is_map()) return Value(static_cast<std::int64_t>(receiver.as_map().size()));
            fail(method, "length expects a string, array, or map");
        }
        if (method.text == "strip") {
            require_arity(method, arguments, 0);
            if (!receiver.is_string()) fail(method, "strip expects a string");
            const auto& input = receiver.as_string();
            const auto first = input.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return Value("");
            const auto last = input.find_last_not_of(" \t\r\n");
            return Value(input.substr(first, last - first + 1));
        }
        if (method.text == "append") {
            require_arity(method, arguments, 1);
            if (!receiver.is_array()) fail(method, "append expects an array receiver");
            receiver.as_array().push_back(arguments[0]);
            return receiver;
        }
        if (method.text == "parse_json" && receiver.is_map()) {
            const auto marker = receiver.as_map().find("__alk_namespace__");
            if (marker != receiver.as_map().end() && marker->second.truthy()) {
                require_arity(method, arguments, 1);
                if (!arguments[0].is_string()) fail(method, "ALK.parse_json expects a string");
                auto result = alk::parse_json(arguments[0].as_string());
                if (!result) throw Failure(result.error());
                return std::move(*result);
            }
        }
        fail(method, "unsupported method '" + method.text + "'");
    }

    static void require_arity(const Token& token, const std::vector<Value>& arguments,
                              std::size_t expected) {
        if (arguments.size() != expected) {
            fail(token, "expected " + std::to_string(expected) + " argument(s), got " +
                            std::to_string(arguments.size()));
        }
    }

    std::string interpolate(const Token& token) {
        std::string output;
        std::size_t cursor = 0;
        while (cursor < token.text.size()) {
            const auto start = token.text.find("#{", cursor);
            if (start == std::string::npos) {
                output.append(token.text.substr(cursor));
                break;
            }
            output.append(token.text.substr(cursor, start - cursor));
            std::size_t end = start + 2;
            int depth = 1;
            char quote = '\0';
            bool escaped = false;
            for (; end < token.text.size(); ++end) {
                const char ch = token.text[end];
                if (quote != '\0') {
                    if (escaped) escaped = false;
                    else if (ch == '\\') escaped = true;
                    else if (ch == quote) quote = '\0';
                    continue;
                }
                if (ch == '"' || ch == '\'') quote = ch;
                else if (ch == '{') ++depth;
                else if (ch == '}' && --depth == 0) break;
            }
            if (depth != 0) fail(token, "unterminated string interpolation");
            const auto source = std::string_view(token.text).substr(start + 2, end - start - 2);
            try {
                output += display(Engine(source, environment_).expression_only());
            } catch (const Failure& nested) {
                fail(token, "invalid interpolation: " + nested.error.message);
            }
            cursor = end + 1;
        }
        return output;
    }

    std::vector<Token> tokens_;
    Environment& environment_;
    std::size_t position_{0};
};

class JsonReader {
public:
    explicit JsonReader(std::string_view source) : source_(source) {}

    Value read() {
        whitespace();
        Value result = value();
        whitespace();
        if (!at_end()) fail("unexpected content after JSON value");
        return result;
    }

private:
    [[nodiscard]] bool at_end() const noexcept { return position_ >= source_.size(); }
    [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : source_[position_]; }
    char advance() {
        const char ch = source_[position_++];
        if (ch == '\n') { ++line_; column_ = 1; } else { ++column_; }
        return ch;
    }
    [[noreturn]] void fail(std::string message) const {
        throw Failure({std::move(message), line_, column_});
    }
    void whitespace() {
        while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') advance();
    }
    bool consume(char expected) {
        if (peek() != expected) return false;
        advance();
        return true;
    }
    void expect(char expected, std::string message) {
        if (!consume(expected)) fail(std::move(message));
    }
    void literal(std::string_view expected) {
        for (const char ch : expected) {
            if (at_end() || advance() != ch) fail("invalid JSON literal");
        }
    }

    Value value() {
        whitespace();
        switch (peek()) {
        case 'n': literal("null"); return Value{};
        case 't': literal("true"); return Value(true);
        case 'f': literal("false"); return Value(false);
        case '"': return Value(string());
        case '[': return array();
        case '{': return object();
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return number();
            fail("expected JSON value");
        }
    }

    static int hex_value(char ch) noexcept {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }
    std::uint32_t hex_quad() {
        std::uint32_t result = 0;
        for (int i = 0; i < 4; ++i) {
            if (at_end()) fail("incomplete Unicode escape");
            const int digit = hex_value(advance());
            if (digit < 0) fail("invalid Unicode escape");
            result = (result << 4U) | static_cast<std::uint32_t>(digit);
        }
        return result;
    }
    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }
    std::string string() {
        expect('"', "expected string");
        std::string result;
        while (!at_end() && peek() != '"') {
            const unsigned char ch = static_cast<unsigned char>(advance());
            if (ch < 0x20U) fail("control character in JSON string");
            if (ch != '\\') { result.push_back(static_cast<char>(ch)); continue; }
            if (at_end()) fail("incomplete JSON escape");
            switch (advance()) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint = hex_quad();
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (!consume('\\') || !consume('u')) fail("missing low Unicode surrogate");
                    const std::uint32_t low = hex_quad();
                    if (low < 0xdc00U || low > 0xdfffU) fail("invalid low Unicode surrogate");
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    fail("unexpected low Unicode surrogate");
                }
                append_utf8(result, codepoint);
                break;
            }
            default: fail("invalid JSON escape");
            }
        }
        expect('"', "unterminated JSON string");
        return result;
    }
    Value number() {
        const auto start = position_;
        consume('-');
        if (consume('0')) {
            if (peek() >= '0' && peek() <= '9') fail("leading zero in JSON number");
        } else {
            if (!(peek() >= '1' && peek() <= '9')) fail("invalid JSON number");
            while (peek() >= '0' && peek() <= '9') advance();
        }
        bool floating = false;
        if (consume('.')) {
            floating = true;
            if (!(peek() >= '0' && peek() <= '9')) fail("expected digits after decimal point");
            while (peek() >= '0' && peek() <= '9') advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            floating = true;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            if (!(peek() >= '0' && peek() <= '9')) fail("expected exponent digits");
            while (peek() >= '0' && peek() <= '9') advance();
        }
        const auto text = source_.substr(start, position_ - start);
        if (!floating) {
            std::int64_t integer{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), integer);
            if (parsed.ec == std::errc{}) return Value(integer);
        }
        double real{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), real);
        if (parsed.ec != std::errc{} || !std::isfinite(real)) fail("JSON number is out of range");
        return Value(real);
    }
    Value array() {
        expect('[', "expected array");
        whitespace();
        Value::Array result;
        if (consume(']')) return Value(std::move(result));
        while (true) {
            result.push_back(value());
            whitespace();
            if (consume(']')) return Value(std::move(result));
            expect(',', "expected ',' between array elements");
            whitespace();
        }
    }
    Value object() {
        expect('{', "expected object");
        whitespace();
        Value::Map result;
        if (consume('}')) return Value(std::move(result));
        while (true) {
            if (peek() != '"') fail("JSON object keys must be strings");
            std::string key = string();
            whitespace();
            expect(':', "expected ':' after object key");
            result.insert_or_assign(std::move(key), value());
            whitespace();
            if (consume('}')) return Value(std::move(result));
            expect(',', "expected ',' between object entries");
            whitespace();
        }
    }

    std::string_view source_;
    std::size_t position_{0};
    std::size_t line_{1};
    std::size_t column_{1};
};

} // namespace

Value::Value() noexcept = default;
Value::Value(std::nullptr_t) noexcept {}
Value::Value(bool value) noexcept : storage_(value) {}
Value::Value(std::int64_t value) noexcept : storage_(value) {}
Value::Value(double value) noexcept : storage_(value) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(std::string_view value) : storage_(std::string(value)) {}
Value::Value(const char* value) : storage_(std::string(value)) {}
Value::Value(Array value) : storage_(std::make_shared<Array>(std::move(value))) {}
Value::Value(Map value) : storage_(std::make_shared<Map>(std::move(value))) {}

bool Value::is_nil() const noexcept { return std::holds_alternative<std::monostate>(storage_); }
bool Value::is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
bool Value::is_int() const noexcept { return std::holds_alternative<std::int64_t>(storage_); }
bool Value::is_float() const noexcept { return std::holds_alternative<double>(storage_); }
bool Value::is_number() const noexcept { return is_int() || is_float(); }
bool Value::is_string() const noexcept { return std::holds_alternative<std::string>(storage_); }
bool Value::is_array() const noexcept { return std::holds_alternative<std::shared_ptr<Array>>(storage_); }
bool Value::is_map() const noexcept { return std::holds_alternative<std::shared_ptr<Map>>(storage_); }
bool Value::truthy() const noexcept { return !is_nil() && (!is_bool() || std::get<bool>(storage_)); }

bool Value::as_bool() const { return std::get<bool>(storage_); }
std::int64_t Value::as_int() const { return std::get<std::int64_t>(storage_); }
double Value::as_float() const {
    return is_int() ? static_cast<double>(as_int()) : std::get<double>(storage_);
}
const std::string& Value::as_string() const { return std::get<std::string>(storage_); }
const Value::Array& Value::as_array() const { return *std::get<std::shared_ptr<Array>>(storage_); }
Value::Array& Value::as_array() { return *std::get<std::shared_ptr<Array>>(storage_); }
const Value::Map& Value::as_map() const { return *std::get<std::shared_ptr<Map>>(storage_); }
Value::Map& Value::as_map() { return *std::get<std::shared_ptr<Map>>(storage_); }

std::string Value::inspect() const {
    if (is_nil()) return "nil";
    if (is_bool()) return as_bool() ? "true" : "false";
    if (is_int()) return std::to_string(as_int());
    if (is_float()) return number_to_string(as_float());
    if (is_string()) return escape_json(as_string());
    if (is_array()) {
        std::string result = "[";
        for (std::size_t i = 0; i < as_array().size(); ++i) {
            if (i != 0) result += ", ";
            result += as_array()[i].inspect();
        }
        return result + "]";
    }
    std::string result = "{";
    bool first = true;
    for (const auto& [key, value] : as_map()) {
        if (!first) result += ", ";
        first = false;
        result += escape_json(key) + ": " + value.inspect();
    }
    return result + "}";
}

std::string Value::to_json() const {
    if (is_nil()) return "null";
    if (is_bool()) return as_bool() ? "true" : "false";
    if (is_int()) return std::to_string(as_int());
    if (is_float()) return number_to_string(as_float());
    if (is_string()) return escape_json(as_string());
    if (is_array()) {
        std::string result = "[";
        for (std::size_t i = 0; i < as_array().size(); ++i) {
            if (i != 0) result.push_back(',');
            result += as_array()[i].to_json();
        }
        return result + "]";
    }
    std::string result = "{";
    bool first = true;
    for (const auto& [key, value] : as_map()) {
        if (!first) result.push_back(',');
        first = false;
        result += escape_json(key) + ":" + value.to_json();
    }
    return result + "}";
}

bool operator==(const Value& lhs, const Value& rhs) {
    if (lhs.is_number() && rhs.is_number()) return lhs.as_float() == rhs.as_float();
    if (lhs.storage_.index() != rhs.storage_.index()) return false;
    if (lhs.is_nil()) return true;
    if (lhs.is_bool()) return lhs.as_bool() == rhs.as_bool();
    if (lhs.is_string()) return lhs.as_string() == rhs.as_string();
    if (lhs.is_array()) return lhs.as_array() == rhs.as_array();
    if (lhs.is_map()) return lhs.as_map() == rhs.as_map();
    return lhs.storage_ == rhs.storage_;
}

class Runtime::Impl {
public:
    detail::AdvancedEngine engine;
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

auto Runtime::execute_script(std::string_view source) -> std::expected<Value, ExecutionError> {
    return impl_->engine.execute(source);
}

auto parse_json(std::string_view source) -> std::expected<Value, ExecutionError> {
    try {
        return JsonReader(source).read();
    } catch (const Failure& failure) {
        return std::unexpected(failure.error);
    } catch (const std::exception& error) {
        return std::unexpected(ExecutionError{error.what(), 1, 1});
    }
}

std::string_view version() noexcept { return "0.3.0-jit-dev"; }

} // namespace alk
