#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace alk {

struct ExecutionError {
    std::string message;
    std::size_t line{1};
    std::size_t column{1};
};

class Value {
public:
    using Array = std::vector<Value>;
    using Map = std::map<std::string, Value, std::less<>>;

    Value() noexcept;
    Value(std::nullptr_t) noexcept;
    Value(bool value) noexcept;
    Value(std::int64_t value) noexcept;
    Value(double value) noexcept;
    Value(std::string value);
    Value(std::string_view value);
    Value(const char* value);
    Value(Array value);
    Value(Map value);

    [[nodiscard]] bool is_nil() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_int() const noexcept;
    [[nodiscard]] bool is_float() const noexcept;
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_map() const noexcept;
    [[nodiscard]] bool truthy() const noexcept;

    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] std::int64_t as_int() const;
    [[nodiscard]] double as_float() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] Array& as_array();
    [[nodiscard]] const Map& as_map() const;
    [[nodiscard]] Map& as_map();

    [[nodiscard]] std::string inspect() const;
    [[nodiscard]] std::string to_json() const;

    friend bool operator==(const Value& lhs, const Value& rhs);

private:
    using Storage = std::variant<std::monostate, bool, std::int64_t, double,
                                 std::string, std::shared_ptr<Array>, std::shared_ptr<Map>>;
    Storage storage_;
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] auto execute_script(std::string_view source)
        -> std::expected<Value, ExecutionError>;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] auto parse_json(std::string_view source)
    -> std::expected<Value, ExecutionError>;

[[nodiscard]] std::string_view version() noexcept;

} // namespace alk
