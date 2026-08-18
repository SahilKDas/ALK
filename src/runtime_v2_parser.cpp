#include "runtime_v2_internal.hpp"

#include <charconv>
#include <cmath>
#include <exception>
#include <limits>
#include <string>

namespace alk::detail {
namespace {

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
    safe_dot,
    dot_dot,
    dot_dot_dot,
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
    greater_equal,
    pipe
};

struct Token {
    TokenKind kind{TokenKind::eof};
    std::string text;
    SourceLocation location;
    bool interpolated{false};
};

struct ParseFailure final : std::exception {
    explicit ParseFailure(ExecutionError value) : error(std::move(value)) {}
    ExecutionError error;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    std::vector<Token> scan() {
        std::vector<Token> result;
        while (!at_end()) {
            const SourceLocation location{line_, column_};
            const char ch = peek();
            if (ch == ' ' || ch == '\t' || ch == '\r') {
                advance();
                continue;
            }
            if (ch == '\n' || ch == ';') {
                advance();
                result.push_back({TokenKind::newline, "\n", location});
                continue;
            }
            if (ch == '#') {
                while (!at_end() && peek() != '\n') advance();
                continue;
            }
            if (identifier_start(ch)) {
                result.push_back(identifier());
                continue;
            }
            if (ch >= '0' && ch <= '9') {
                result.push_back(number());
                continue;
            }
            if (ch == '"' || ch == '\'') {
                result.push_back(string());
                continue;
            }
            advance();
            switch (ch) {
            case '(': result.push_back({TokenKind::left_paren, "(", location}); break;
            case ')': result.push_back({TokenKind::right_paren, ")", location}); break;
            case '[': result.push_back({TokenKind::left_bracket, "[", location}); break;
            case ']': result.push_back({TokenKind::right_bracket, "]", location}); break;
            case '{': result.push_back({TokenKind::left_brace, "{", location}); break;
            case '}': result.push_back({TokenKind::right_brace, "}", location}); break;
            case ',': result.push_back({TokenKind::comma, ",", location}); break;
            case ':': result.push_back({TokenKind::colon, ":", location}); break;
            case '+': result.push_back({TokenKind::plus, "+", location}); break;
            case '-': result.push_back({TokenKind::minus, "-", location}); break;
            case '*': result.push_back({TokenKind::star, "*", location}); break;
            case '/': result.push_back({TokenKind::slash, "/", location}); break;
            case '%': result.push_back({TokenKind::percent, "%", location}); break;
            case '|': result.push_back({TokenKind::pipe, "|", location}); break;
            case '?':
                if (match('.')) result.push_back({TokenKind::safe_dot, "?.", location});
                else fail(location, "expected '.' after '?'");
                break;
            case '.':
                if (match('.')) {
                    if (match('.')) result.push_back({TokenKind::dot_dot_dot, "...", location});
                    else result.push_back({TokenKind::dot_dot, "..", location});
                } else {
                    result.push_back({TokenKind::dot, ".", location});
                }
                break;
            case '!':
                result.push_back(match('=')
                    ? Token{TokenKind::bang_equal, "!=", location}
                    : Token{TokenKind::bang, "!", location});
                break;
            case '=':
                result.push_back(match('=')
                    ? Token{TokenKind::equal_equal, "==", location}
                    : Token{TokenKind::equal, "=", location});
                break;
            case '<':
                result.push_back(match('=')
                    ? Token{TokenKind::less_equal, "<=", location}
                    : Token{TokenKind::less, "<", location});
                break;
            case '>':
                result.push_back(match('=')
                    ? Token{TokenKind::greater_equal, ">=", location}
                    : Token{TokenKind::greater, ">", location});
                break;
            default: fail(location, "unexpected character");
            }
        }
        result.push_back({TokenKind::eof, "", {line_, column_}});
        return result;
    }

private:
    bool at_end() const noexcept { return position_ >= source_.size(); }
    char peek(std::size_t offset = 0) const noexcept {
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
    [[noreturn]] static void fail(SourceLocation location, std::string message) {
        throw ParseFailure({std::move(message), location.line, location.column});
    }
    static bool identifier_start(char ch) noexcept {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    }
    static bool identifier_continue(char ch) noexcept {
        return identifier_start(ch) || (ch >= '0' && ch <= '9');
    }
    static int hex_value(char ch) noexcept {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }
    std::uint32_t unicode_escape(SourceLocation location) {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            if (at_end()) fail(location, "incomplete Unicode escape");
            const int digit = hex_value(advance());
            if (digit < 0) fail(location, "invalid Unicode escape");
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        return value;
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
        const SourceLocation location{line_, column_};
        advance();
        while (identifier_continue(peek())) advance();
        if (peek() == '!' || (peek() == '?' && peek(1) != '.')) advance();
        return {TokenKind::identifier, std::string(source_.substr(start, position_ - start)), location};
    }

    Token number() {
        const auto start = position_;
        const SourceLocation location{line_, column_};
        while (peek() >= '0' && peek() <= '9') advance();
        if (peek() == '.' && peek(1) != '.' && peek(1) >= '0' && peek(1) <= '9') {
            advance();
            while (peek() >= '0' && peek() <= '9') advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            advance();
            if (peek() == '+' || peek() == '-') advance();
            if (!(peek() >= '0' && peek() <= '9')) fail(location, "expected exponent digits");
            while (peek() >= '0' && peek() <= '9') advance();
        }
        return {TokenKind::number, std::string(source_.substr(start, position_ - start)), location};
    }

    Token string() {
        const SourceLocation location{line_, column_};
        const char quote = advance();
        std::string value;
        bool interpolated = false;
        int interpolation_depth = 0;
        while (!at_end()) {
            if (interpolation_depth == 0 && peek() == quote) break;
            if (peek() == '\n' && interpolation_depth == 0) fail(location, "unterminated string");
            char ch = advance();
            if (quote == '"' && ch == '#' && peek() == '{') {
                interpolated = true;
                interpolation_depth = 1;
                value += "#{";
                advance();
                continue;
            }
            if (interpolation_depth > 0) {
                if (ch == '{') ++interpolation_depth;
                if (ch == '}') --interpolation_depth;
                value.push_back(ch);
                continue;
            }
            if (ch != '\\') {
                value.push_back(ch);
                continue;
            }
            if (at_end()) fail(location, "incomplete string escape");
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
                std::uint32_t codepoint = unicode_escape(location);
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    if (!match('\\') || !match('u')) fail(location, "missing low Unicode surrogate");
                    const auto low = unicode_escape(location);
                    if (low < 0xdc00U || low > 0xdfffU) fail(location, "invalid low Unicode surrogate");
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    fail(location, "unexpected low Unicode surrogate");
                }
                append_utf8(value, codepoint);
                break;
            }
            default: fail(location, "unsupported string escape");
            }
        }
        if (at_end() || interpolation_depth != 0) fail(location, "unterminated string");
        advance();
        return {TokenKind::string, std::move(value), location, interpolated};
    }

    std::string_view source_;
    std::size_t position_{};
    std::size_t line_{1};
    std::size_t column_{1};
};

class Parser {
public:
    explicit Parser(std::string_view source) : tokens_(Lexer(source).scan()) {}

    ParseResult parse() {
        ParseResult result;
        skip_newlines();
        while (!check(TokenKind::eof)) {
            result.statements.push_back(statement());
            require_separator();
            skip_newlines();
        }
        return result;
    }

private:
    const Token& current() const noexcept { return tokens_[position_]; }
    const Token& previous() const noexcept { return tokens_[position_ - 1]; }
    bool check(TokenKind kind) const noexcept { return current().kind == kind; }
    bool keyword(std::string_view value) const noexcept {
        return check(TokenKind::identifier) && current().text == value;
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
    bool match_keyword(std::string_view value) noexcept {
        if (!keyword(value)) return false;
        advance();
        return true;
    }
    const Token& consume(TokenKind kind, std::string message) {
        if (check(kind)) return advance();
        fail(current(), std::move(message));
    }
    const Token& consume_identifier(std::string message) {
        return consume(TokenKind::identifier, std::move(message));
    }
    [[noreturn]] static void fail(const Token& token, std::string message) {
        throw ParseFailure({std::move(message), token.location.line, token.location.column});
    }
    void skip_newlines() noexcept { while (match(TokenKind::newline)) {} }
    void require_separator() {
        if (!check(TokenKind::newline) && !check(TokenKind::eof) &&
            !keyword("end") && !keyword("rescue")) {
            fail(current(), "expected a new line after statement");
        }
    }
    static ExprPtr make_expr(ExprKind kind, SourceLocation location) {
        auto expression = std::make_shared<Expr>();
        expression->kind = kind;
        expression->location = location;
        return expression;
    }
    static StmtPtr make_stmt(StmtKind kind, SourceLocation location) {
        auto statement = std::make_shared<Stmt>();
        statement->kind = kind;
        statement->location = location;
        return statement;
    }

    StatementList body_until(std::initializer_list<std::string_view> endings) {
        StatementList body;
        skip_newlines();
        auto is_ending = [&]() {
            for (const auto ending : endings) if (keyword(ending)) return true;
            return false;
        };
        while (!check(TokenKind::eof) && !is_ending()) {
            body.push_back(statement());
            require_separator();
            skip_newlines();
        }
        if (check(TokenKind::eof)) fail(current(), "unterminated block");
        return body;
    }

    StmtPtr statement() {
        if (match_keyword("def")) return function_definition(previous().location);
        if (match_keyword("class")) return class_definition(previous().location);
        if (match_keyword("module")) return module_definition(previous().location);
        if (match_keyword("include")) {
            auto result = make_stmt(StmtKind::include_module, previous().location);
            result->name = consume_identifier("expected module name").text;
            return result;
        }
        if (match_keyword("prepend")) {
            auto result = make_stmt(StmtKind::prepend_module, previous().location);
            result->name = consume_identifier("expected module name").text;
            return result;
        }
        if (match_keyword("import")) return import_statement(previous().location);
        if (match_keyword("begin")) return rescue_statement(previous().location);
        if (match_keyword("if")) return conditional_statement(previous().location, false);
        if (match_keyword("unless")) return conditional_statement(previous().location, true);
        if (match_keyword("while")) return while_statement(previous().location);
        if (match_keyword("return")) {
            auto result = make_stmt(StmtKind::return_value, previous().location);
            if (!check(TokenKind::newline) && !keyword("end")) result->expression = expression();
            return postfix_condition(std::move(result));
        }
        if (match_keyword("raise")) {
            auto result = make_stmt(StmtKind::raise_value, previous().location);
            result->expression = expression();
            return postfix_condition(std::move(result));
        }
        if (match_keyword("puts")) {
            auto result = make_stmt(StmtKind::print, previous().location);
            result->expression = expression();
            return postfix_condition(std::move(result));
        }

        ExprPtr left = expression();
        if (match(TokenKind::equal)) {
            const auto location = previous().location;
            ExprPtr value = expression();
            if (left->kind == ExprKind::variable) {
                auto result = make_stmt(StmtKind::assignment, location);
                result->name = left->text;
                result->expression = std::move(value);
                return postfix_condition(std::move(result));
            }
            if (left->kind == ExprKind::property || left->kind == ExprKind::safe_property) {
                if (left->kind == ExprKind::safe_property) fail(previous(), "cannot assign through safe navigation");
                auto result = make_stmt(StmtKind::property_assignment, location);
                result->receiver = left->left;
                result->name = left->text;
                result->expression = std::move(value);
                return postfix_condition(std::move(result));
            }
            if (left->kind == ExprKind::index) {
                auto result = make_stmt(StmtKind::index_assignment, location);
                result->receiver = left->left;
                result->index = left->right;
                result->expression = std::move(value);
                return postfix_condition(std::move(result));
            }
            fail(previous(), "invalid assignment target");
        }
        auto result = make_stmt(StmtKind::expression, left->location);
        result->expression = std::move(left);
        return postfix_condition(std::move(result));
    }

    StmtPtr postfix_condition(StmtPtr inner) {
        bool inverted = false;
        SourceLocation location = inner->location;
        if (match_keyword("if")) location = previous().location;
        else if (match_keyword("unless")) { location = previous().location; inverted = true; }
        else return inner;
        auto result = make_stmt(StmtKind::conditional, location);
        result->name = inverted ? "unless" : "if";
        result->expression = expression();
        result->body.push_back(std::move(inner));
        return result;
    }

    StmtPtr conditional_statement(SourceLocation location, bool inverted) {
        auto result = make_stmt(StmtKind::conditional, location);
        result->name = inverted ? "unless" : "if";
        result->expression = expression();
        consume_block_separator();
        result->body = body_until({"else", "end"});
        if (match_keyword("else")) {
            consume_block_separator();
            result->rescue_body = body_until({"end"});
        }
        if (!match_keyword("end")) fail(current(), "expected 'end' after conditional");
        return result;
    }

    StmtPtr while_statement(SourceLocation location) {
        auto result = make_stmt(StmtKind::while_loop, location);
        result->expression = expression();
        consume_block_separator();
        result->body = body_until({"end"});
        if (!match_keyword("end")) fail(current(), "expected 'end' after while loop");
        return result;
    }

    StmtPtr function_definition(SourceLocation location) {
        auto result = make_stmt(StmtKind::function_definition, location);
        result->name = consume_identifier("expected function name").text;
        consume(TokenKind::left_paren, "expected '(' after function name");
        if (!check(TokenKind::right_paren)) {
            do { result->names.push_back(consume_identifier("expected parameter name").text); }
            while (match(TokenKind::comma));
        }
        consume(TokenKind::right_paren, "expected ')' after parameters");
        consume_block_separator();
        result->body = body_until({"end"});
        advance();
        return result;
    }

    StmtPtr class_definition(SourceLocation location) {
        auto result = make_stmt(StmtKind::class_definition, location);
        result->name = consume_identifier("expected class name").text;
        if (match(TokenKind::less)) result->secondary_name = consume_identifier("expected parent class name").text;
        consume_block_separator();
        result->body = body_until({"end"});
        advance();
        return result;
    }

    StmtPtr module_definition(SourceLocation location) {
        auto result = make_stmt(StmtKind::module_definition, location);
        result->name = consume_identifier("expected module name").text;
        consume_block_separator();
        result->body = body_until({"end"});
        advance();
        return result;
    }

    StmtPtr rescue_statement(SourceLocation location) {
        auto result = make_stmt(StmtKind::rescue_block, location);
        consume_block_separator();
        result->body = body_until({"rescue", "end"});
        if (match_keyword("rescue")) {
            if (check(TokenKind::identifier)) result->name = advance().text;
            consume_block_separator();
            result->rescue_body = body_until({"end"});
        }
        if (!match_keyword("end")) fail(current(), "expected 'end' after rescue block");
        return result;
    }

    StmtPtr import_statement(SourceLocation location) {
        auto result = make_stmt(StmtKind::import_names, location);
        result->name = consume_identifier("expected module path").text;
        while (match(TokenKind::dot)) {
            result->name += ".";
            result->name += consume_identifier("expected module path component").text;
        }
        consume(TokenKind::left_brace, "expected '{' before imported names");
        if (!check(TokenKind::right_brace)) {
            do { result->names.push_back(consume_identifier("expected imported name").text); }
            while (match(TokenKind::comma));
        }
        consume(TokenKind::right_brace, "expected '}' after imported names");
        return result;
    }

    void consume_block_separator() {
        if (!match(TokenKind::newline)) fail(current(), "expected new line before block body");
        skip_newlines();
    }

    ExprPtr expression() { return logical_or(); }
    ExprPtr logical_or() {
        ExprPtr left = logical_and();
        while (match_keyword("or")) left = binary(previous(), "or", std::move(left), logical_and());
        return left;
    }
    ExprPtr logical_and() {
        ExprPtr left = equality();
        while (match_keyword("and")) left = binary(previous(), "and", std::move(left), equality());
        return left;
    }
    ExprPtr equality() {
        ExprPtr left = comparison();
        while (check(TokenKind::equal_equal) || check(TokenKind::bang_equal)) {
            const Token op = advance();
            left = binary(op, op.text, std::move(left), comparison());
        }
        return left;
    }
    ExprPtr comparison() {
        ExprPtr left = range();
        while (check(TokenKind::less) || check(TokenKind::less_equal) ||
               check(TokenKind::greater) || check(TokenKind::greater_equal)) {
            const Token op = advance();
            left = binary(op, op.text, std::move(left), range());
        }
        return left;
    }
    ExprPtr range() {
        ExprPtr left = term();
        if (match(TokenKind::dot_dot) || match(TokenKind::dot_dot_dot)) {
            const Token op = previous();
            auto result = make_expr(ExprKind::range, op.location);
            result->text = op.text;
            result->left = std::move(left);
            if (!check(TokenKind::newline) && !check(TokenKind::comma) &&
                !check(TokenKind::right_paren) && !check(TokenKind::right_bracket) &&
                !keyword("do") && !keyword("end")) {
                result->right = term();
            }
            return result;
        }
        return left;
    }
    ExprPtr term() {
        ExprPtr left = factor();
        while (check(TokenKind::plus) || check(TokenKind::minus)) {
            const Token op = advance();
            left = binary(op, op.text, std::move(left), factor());
        }
        return left;
    }
    ExprPtr factor() {
        ExprPtr left = unary();
        while (check(TokenKind::star) || check(TokenKind::slash) || check(TokenKind::percent)) {
            const Token op = advance();
            left = binary(op, op.text, std::move(left), unary());
        }
        return left;
    }
    ExprPtr unary() {
        if (match(TokenKind::minus) || match(TokenKind::bang) || match_keyword("not")) {
            const Token op = previous();
            auto result = make_expr(ExprKind::unary, op.location);
            result->text = op.text;
            result->right = unary();
            return result;
        }
        return postfix();
    }

    ExprPtr postfix() {
        ExprPtr result = primary();
        while (true) {
            if (match(TokenKind::left_paren)) {
                auto call = make_expr(ExprKind::call, previous().location);
                call->left = std::move(result);
                call->arguments = arguments();
                result = std::move(call);
                continue;
            }
            if (match(TokenKind::left_bracket)) {
                auto index = make_expr(ExprKind::index, previous().location);
                index->left = std::move(result);
                index->right = expression();
                consume(TokenKind::right_bracket, "expected ']' after index");
                result = std::move(index);
                continue;
            }
            if (match(TokenKind::dot) || match(TokenKind::safe_dot)) {
                const bool safe = previous().kind == TokenKind::safe_dot;
                const Token name = consume_identifier("expected property or method name");
                auto property = make_expr(safe ? ExprKind::safe_property : ExprKind::property, name.location);
                property->left = std::move(result);
                property->text = name.text;
                result = std::move(property);
                continue;
            }
            break;
        }
        if (match_keyword("do")) {
            if (result->kind != ExprKind::call) {
                auto call = make_expr(ExprKind::call, previous().location);
                call->left = std::move(result);
                result = std::move(call);
            }
            if (match(TokenKind::pipe)) {
                if (!check(TokenKind::pipe)) {
                    do { result->block_parameters.push_back(consume_identifier("expected block parameter").text); }
                    while (match(TokenKind::comma));
                }
                consume(TokenKind::pipe, "expected '|' after block parameters");
            }
            consume_block_separator();
            result->block_body = body_until({"end"});
            advance();
        }
        return result;
    }

    std::vector<ExprPtr> arguments() {
        std::vector<ExprPtr> result;
        skip_newlines();
        if (!check(TokenKind::right_paren)) {
            do {
                skip_newlines();
                result.push_back(expression());
                skip_newlines();
            } while (match(TokenKind::comma));
        }
        consume(TokenKind::right_paren, "expected ')' after arguments");
        return result;
    }

    ExprPtr primary() {
        if (match_keyword("nil") || match_keyword("null")) {
            auto result = make_expr(ExprKind::literal, previous().location);
            result->literal = Dynamic{};
            return result;
        }
        if (match_keyword("true") || match_keyword("false")) {
            const Token value = previous();
            auto result = make_expr(ExprKind::literal, value.location);
            result->literal = Dynamic(value.text == "true");
            return result;
        }
        if (match(TokenKind::number)) {
            const Token value = previous();
            auto result = make_expr(ExprKind::literal, value.location);
            if (value.text.find_first_of(".eE") == std::string::npos) {
                std::int64_t integer{};
                const auto parsed = std::from_chars(value.text.data(), value.text.data() + value.text.size(), integer);
                if (parsed.ec == std::errc{}) {
                    result->literal = Dynamic(integer);
                    return result;
                }
            }
            double real{};
            const auto parsed = std::from_chars(value.text.data(), value.text.data() + value.text.size(), real);
            if (parsed.ec != std::errc{} || !std::isfinite(real)) fail(value, "invalid number");
            result->literal = Dynamic(real);
            return result;
        }
        if (match(TokenKind::string)) {
            const Token value = previous();
            auto result = make_expr(ExprKind::literal, value.location);
            result->literal = Dynamic(value.text);
            if (value.interpolated) result->text = "interpolated";
            return result;
        }
        if (match(TokenKind::identifier)) {
            const Token name = previous();
            auto result = make_expr(ExprKind::variable, name.location);
            result->text = name.text;
            return result;
        }
        if (match(TokenKind::left_paren)) {
            ExprPtr result = expression();
            consume(TokenKind::right_paren, "expected ')' after expression");
            return result;
        }
        if (match(TokenKind::left_bracket)) return array_literal(previous().location);
        if (match(TokenKind::left_brace)) return map_literal(previous().location);
        fail(current(), "expected expression");
    }

    ExprPtr array_literal(SourceLocation location) {
        auto result = make_expr(ExprKind::array, location);
        skip_newlines();
        if (match(TokenKind::right_bracket)) return result;
        ExprPtr first = expression();
        if (match_keyword("for")) {
            result->kind = ExprKind::comprehension;
            result->left = std::move(first);
            result->text = consume_identifier("expected comprehension variable").text;
            if (!match_keyword("in")) fail(current(), "expected 'in' in comprehension");
            result->right = expression();
            if (match_keyword("if")) result->third = expression();
            consume(TokenKind::right_bracket, "expected ']' after comprehension");
            return result;
        }
        result->items.push_back(std::move(first));
        while (match(TokenKind::comma)) {
            skip_newlines();
            if (check(TokenKind::right_bracket)) break;
            result->items.push_back(expression());
        }
        skip_newlines();
        consume(TokenKind::right_bracket, "expected ']' after array");
        return result;
    }

    ExprPtr map_literal(SourceLocation location) {
        auto result = make_expr(ExprKind::map, location);
        skip_newlines();
        if (match(TokenKind::right_brace)) return result;
        do {
            skip_newlines();
            const Token key = consume(TokenKind::string, "map keys must be strings");
            consume(TokenKind::colon, "expected ':' after map key");
            skip_newlines();
            result->entries.emplace_back(key.text, expression());
            skip_newlines();
        } while (match(TokenKind::comma) && !check(TokenKind::right_brace));
        consume(TokenKind::right_brace, "expected '}' after map");
        return result;
    }

    static ExprPtr binary(const Token& token, std::string operation, ExprPtr left, ExprPtr right) {
        auto result = make_expr(ExprKind::binary, token.location);
        result->text = std::move(operation);
        result->left = std::move(left);
        result->right = std::move(right);
        return result;
    }

    std::vector<Token> tokens_;
    std::size_t position_{};
};

} // namespace

auto parse_program(std::string_view source) -> std::expected<ParseResult, ExecutionError> {
    try {
        return Parser(source).parse();
    } catch (const ParseFailure& failure) {
        return std::unexpected(failure.error);
    } catch (const std::exception& error) {
        return std::unexpected(ExecutionError{error.what(), 1, 1});
    }
}

} // namespace alk::detail
