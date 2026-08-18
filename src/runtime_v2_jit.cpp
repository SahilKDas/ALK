#include "runtime_v2_internal.hpp"

#include <cstring>
#include <limits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace alk::detail {

struct NativeCode {
    using Entry = std::int64_t (*)(const std::int64_t*, std::uint8_t*);
    void* memory{};
    std::size_t allocation_size{};
    std::size_t argument_count{};
    Entry entry{};

    ~NativeCode() {
#if defined(_WIN32)
        if (memory) VirtualFree(memory, 0, MEM_RELEASE);
#elif defined(__unix__) || defined(__APPLE__)
        if (memory) munmap(memory, allocation_size);
#endif
    }
};

namespace {

#if (defined(__x86_64__) || defined(_M_X64)) && \
    (defined(_WIN32) || defined(__unix__) || defined(__APPLE__))
constexpr bool supported = true;
#else
constexpr bool supported = false;
#endif

void byte(std::vector<std::uint8_t>& code, std::uint8_t value) { code.push_back(value); }
void u32(std::vector<std::uint8_t>& code, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        byte(code, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}
void u64(std::vector<std::uint8_t>& code, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        byte(code, static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

std::uint32_t slot(std::uint32_t index) { return index * 8U; }

void load_stack_rax(std::vector<std::uint8_t>& code, std::uint32_t index) {
    byte(code, 0x48); byte(code, 0x8b); byte(code, 0x84); byte(code, 0x24); u32(code, slot(index));
}
void load_stack_rdx(std::vector<std::uint8_t>& code, std::uint32_t index) {
    byte(code, 0x48); byte(code, 0x8b); byte(code, 0x94); byte(code, 0x24); u32(code, slot(index));
}
void store_rax(std::vector<std::uint8_t>& code, std::uint32_t index) {
    byte(code, 0x48); byte(code, 0x89); byte(code, 0x84); byte(code, 0x24); u32(code, slot(index));
}
void overflow_status(std::vector<std::uint8_t>& code) {
    byte(code, 0x71); byte(code, 0x04);             // jno +4
    byte(code, 0x41); byte(code, 0xc6); byte(code, 0x03); byte(code, 0x01); // [r11] = 1
}

bool eligible(const BytecodeChunk& chunk) {
    if (chunk.register_count == 0 || chunk.register_count > 4096U) return false;
    for (const auto& constant : chunk.constants) if (!constant.is_int()) return false;
    for (const auto& instruction : chunk.code) {
        switch (instruction.op) {
        case OpCode::load_constant:
        case OpCode::load_parameter:
        case OpCode::move:
        case OpCode::add:
        case OpCode::subtract:
        case OpCode::multiply:
        case OpCode::negate:
        case OpCode::return_value:
            break;
        default:
            return false;
        }
    }
    return true;
}

std::shared_ptr<NativeCode> finalize(std::vector<std::uint8_t> code,
                                    std::size_t argument_count) {
    if (code.empty()) return {};
    auto result = std::make_shared<NativeCode>();
    result->argument_count = argument_count;
#if defined(_WIN32)
    result->allocation_size = code.size();
    result->memory = VirtualAlloc(nullptr, result->allocation_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!result->memory) return {};
    std::memcpy(result->memory, code.data(), code.size());
    DWORD previous{};
    if (!VirtualProtect(result->memory, result->allocation_size, PAGE_EXECUTE_READ, &previous)) return {};
    FlushInstructionCache(GetCurrentProcess(), result->memory, result->allocation_size);
#elif defined(__unix__) || defined(__APPLE__)
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return {};
    const auto page_size = static_cast<std::size_t>(page);
    result->allocation_size = ((code.size() + page_size - 1U) / page_size) * page_size;
    result->memory = mmap(nullptr, result->allocation_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result->memory == MAP_FAILED) { result->memory = nullptr; return {}; }
    std::memcpy(result->memory, code.data(), code.size());
    if (mprotect(result->memory, result->allocation_size, PROT_READ | PROT_EXEC) != 0) return {};
    __builtin___clear_cache(static_cast<char*>(result->memory),
                            static_cast<char*>(result->memory) + code.size());
#else
    static_cast<void>(code);
    return {};
#endif
    result->entry = reinterpret_cast<NativeCode::Entry>(result->memory);
    return result;
}

} // namespace

bool native_available() noexcept { return supported; }

std::shared_ptr<NativeCode> compile_native(const BytecodeChunk& chunk) {
    if constexpr (!supported) {
        static_cast<void>(chunk);
        return {};
    } else {
        if (!eligible(chunk) || !verify_bytecode(chunk)) return {};
        std::vector<std::uint8_t> code;
        code.reserve(chunk.code.size() * 24U + 32U);

#if defined(_WIN32)
        byte(code, 0x49); byte(code, 0x89); byte(code, 0xca); // mov r10, rcx (arguments)
        byte(code, 0x49); byte(code, 0x89); byte(code, 0xd3); // mov r11, rdx (status)
#else
        byte(code, 0x49); byte(code, 0x89); byte(code, 0xfa); // mov r10, rdi
        byte(code, 0x49); byte(code, 0x89); byte(code, 0xf3); // mov r11, rsi
#endif
        const std::uint32_t raw_frame = chunk.register_count * 8U;
        const std::uint32_t frame = (raw_frame + 15U) & ~15U;
        byte(code, 0x48); byte(code, 0x81); byte(code, 0xec); u32(code, frame); // sub rsp, frame

        bool returned = false;
        for (const auto& instruction : chunk.code) {
            switch (instruction.op) {
            case OpCode::load_constant:
                byte(code, 0x48); byte(code, 0xb8);
                u64(code, static_cast<std::uint64_t>(chunk.constants[instruction.b].integer()));
                store_rax(code, instruction.a);
                break;
            case OpCode::load_parameter:
                byte(code, 0x49); byte(code, 0x8b); byte(code, 0x82);
                u32(code, instruction.b * 8U);
                store_rax(code, instruction.a);
                break;
            case OpCode::move:
                load_stack_rax(code, instruction.b);
                store_rax(code, instruction.a);
                break;
            case OpCode::add:
            case OpCode::subtract:
            case OpCode::multiply:
                load_stack_rax(code, instruction.b);
                load_stack_rdx(code, instruction.c);
                if (instruction.op == OpCode::add) {
                    byte(code, 0x48); byte(code, 0x01); byte(code, 0xd0); // add rax, rdx
                } else if (instruction.op == OpCode::subtract) {
                    byte(code, 0x48); byte(code, 0x29); byte(code, 0xd0); // sub rax, rdx
                } else {
                    byte(code, 0x48); byte(code, 0x0f); byte(code, 0xaf); byte(code, 0xc2); // imul rax, rdx
                }
                overflow_status(code);
                store_rax(code, instruction.a);
                break;
            case OpCode::negate:
                load_stack_rax(code, instruction.b);
                byte(code, 0x48); byte(code, 0xf7); byte(code, 0xd8); // neg rax
                overflow_status(code);
                store_rax(code, instruction.a);
                break;
            case OpCode::return_value:
                load_stack_rax(code, instruction.a);
                byte(code, 0x48); byte(code, 0x81); byte(code, 0xc4); u32(code, frame); // add rsp, frame
                byte(code, 0xc3); // ret
                returned = true;
                break;
            default:
                return {};
            }
        }
        if (!returned) return {};
        return finalize(std::move(code), chunk.parameter_count);
    }
}

std::optional<std::int64_t> invoke_native(const std::shared_ptr<NativeCode>& code,
                                          const std::vector<Dynamic>& arguments) {
    if (!code || !code->entry || arguments.size() != code->argument_count) return {};
    std::vector<std::int64_t> integers;
    integers.reserve(arguments.size());
    for (const auto& argument : arguments) {
        if (!argument.is_int()) return {};
        integers.push_back(argument.integer());
    }
    std::uint8_t status = 0;
    const std::int64_t result = code->entry(integers.data(), &status);
    if (status != 0) return {};
    return result;
}

} // namespace alk::detail
