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

void test_values_and_expressions() {
    alk::Runtime runtime;
    check(run(runtime, "1 + 2 * 3") == alk::Value(std::int64_t{7}), "operator precedence");
    check(run(runtime, "(1 + 2) * 3") == alk::Value(std::int64_t{9}), "parentheses");
    check(run(runtime, "not false and 4 >= 4") == alk::Value(true), "logic and comparison");
    check(run(runtime, "name = \"ALK\"\n\"Hello, #{name}!\"") == alk::Value("Hello, ALK!"),
          "Ruby-style interpolation");
    check(run(runtime, "data = {\"name\": \"ALK\"}\n\"Hello, #{data['name']}!\"") ==
              alk::Value("Hello, ALK!"), "indexing inside interpolation");
}

void test_collections() {
    alk::Runtime runtime;
    check(run(runtime, "values = [10, 20]\nvalues.append(30)\nvalues[2]") ==
              alk::Value(std::int64_t{30}), "array append and indexing");
    check(run(runtime, "data = {\"user\": {\"name\": \"Alex\"}}\ndata[\"user\"][\"name\"]") ==
              alk::Value("Alex"), "map navigation");
    check(run(runtime, "\"  text  \".strip().length()") == alk::Value(std::int64_t{4}),
          "string methods");
}

void test_json() {
    constexpr std::string_view source =
        R"({"user":{"name":"Alex","roles":["admin","developer"],"active":true,"note":null},"n":1.5e2})";
    auto parsed = alk::parse_json(source);
    check(parsed.has_value(), "valid JSON parses");
    if (parsed) {
        check(parsed->as_map().at("user").as_map().at("note").is_nil(), "JSON null maps to nil");
        auto reparsed = alk::parse_json(parsed->to_json());
        check(reparsed.has_value() && *reparsed == *parsed, "JSON round trip");
    }

    auto unicode = alk::parse_json(R"("\u0041\ud83d\ude80")");
    check(unicode.has_value() && unicode->as_string() == "A\xF0\x9F\x9A\x80", "Unicode escapes");
    check(!alk::parse_json("{unquoted: 1}"), "invalid JSON is rejected");

    alk::Runtime runtime;
    check(run(runtime, R"(ALK.parse_json("{\"ok\":true}")["ok"])") == alk::Value(true),
          "ALK.parse_json binding");
    check(run(runtime, R"({"a": 1, "b": nil}.to_json())") == alk::Value(R"({"a":1,"b":null})"),
          "native ALK literal serialization");
    check(run(runtime, R"({"letter":"\u0041","rocket":"\ud83d\ude80"}.to_json())") ==
              alk::Value("{\"letter\":\"A\",\"rocket\":\"\xF0\x9F\x9A\x80\"}"),
          "valid JSON Unicode escapes are valid ALK source literals");
}

void test_errors() {
    alk::Runtime runtime;
    auto undefined = runtime.execute_script("missing_name");
    check(!undefined && undefined.error().line == 1 && undefined.error().column == 1,
          "errors carry source positions");
    check(!runtime.execute_script("1 / 0"), "division by zero is rejected");
}

} // namespace

int main() {
    test_values_and_expressions();
    test_collections();
    test_json();
    test_errors();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All ALK runtime tests passed\n";
    return EXIT_SUCCESS;
}
