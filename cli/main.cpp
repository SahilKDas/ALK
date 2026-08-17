#include <alk/alk.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void usage(std::ostream& output) {
    output << "ALK " << alk::version() << "\n"
           << "Usage:\n"
           << "  alk <file.alk>\n"
           << "  alk -e <source>\n"
           << "  alk --version\n";
}

int execute(std::string_view source) {
    alk::Runtime runtime;
    auto result = runtime.execute_script(source);
    if (!result) {
        std::cerr << "ALK error at " << result.error().line << ':' << result.error().column
                  << ": " << result.error().message << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << alk::version() << '\n';
        return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "-e") {
        return execute(argv[2]);
    }
    if (argc != 2) {
        usage(std::cerr);
        return 2;
    }

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "ALK: cannot open '" << argv[1] << "'\n";
        return 2;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return execute(buffer.str());
}
