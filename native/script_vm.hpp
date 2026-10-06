#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace yami::script {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Jump-table order at 0040d7ac; operands are raw little-endian words, NOT floats.
// Binary operations consume the top operand FIRST: top - next, top / next, etc.
enum class Opcode : std::uint32_t {
    PushWord, PushFloat, PushLocal, PushIndirect, PushAddress, PushString,
    StoreWord, StoreFloat, AddInt, MultiplyInt, SubtractInt, DivideInt,
    AddFloat, MultiplyFloat, SubtractFloat, DivideFloat,
    LessInt, LessEqualInt, EqualInt, NotEqualInt, GreaterEqualInt, GreaterInt,
    LessFloat, LessEqualFloat, EqualFloat, NotEqualFloat, GreaterEqualFloat, GreaterFloat,
    JumpIfZero, JumpIfNonzero, Jump, IntToFloat, FloatToInt,
    ResolveWord, ResolveFloat, Call, And, Or, NegateInt, NegateFloat
};

struct Instruction {
    Opcode opcode{};
    std::uint32_t argument{};
    std::uint32_t word_offset{};
    std::size_t jump_target{}; // Decoded instruction index, including end-of-function.
    std::string text;         // Original name bytes; no locale/Unicode conversion.
};

struct Function {
    std::string name;
    std::vector<std::uint32_t> initial_data;
    std::vector<Instruction> instructions;
    std::uint32_t code_word_count{};
};

struct Record {
    // PCS tag 0: function (name, data count/data, code count/code).
    // PCS tag 1: include (name only). There is no header or terminator.
    bool include{};
    std::string name;
    Function function;
};

struct Program {
    std::string source;
    std::vector<Record> records; // Order matters; original registry is first-insertion-wins.
    static Program decode(const std::vector<std::uint8_t>& bytes, std::string source = {});
    static Program read(std::istream& input, std::string source = {});
    static Program read_file(const std::filesystem::path& path);
};

const char* opcode_name(Opcode opcode);
std::uint32_t opcode_address(Opcode opcode);

enum class Argument { None, Word, Int, Unsigned, Float, String };
struct HostSpec {
    const char* name;
    std::uint32_t address;
    std::array<Argument, 3> pop_order; // Host consumes these in order, from the global stack.
    Argument result;
};
const std::array<HostSpec, 20>& game_host_specs(); // Registry 00415db0, no fake implementations.

class Runtime {
public:
    using Host = std::function<void(Runtime&)>;
    using IncludeLoader = std::function<Program(const std::string&)>;
    enum class Result { Missing, Completed, RegistryCleared };
    // Original stack [004693b0,00469400) has 20 words = 10 payload/tag pairs.
    static constexpr std::size_t stack_capacity = 10;

    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Duplicate names do not replace the first callable (0040eb40).
    bool register_host(std::string name, Host host);
    void load(const Program& program, const IncludeLoader& includes = {});
    // Includes always resolve against this directory, not against the including file.
    // The original loader constructs data/scripts/<name>.pcs (0040dbf0).
    void load_directory(const std::filesystem::path& directory, const std::string& name);
    bool contains(std::string_view name) const;
    // Save words follow insertion order, never std::map's lexical order.
    // Views are invalidated by registry mutation.
    std::span<const std::string_view> ordered_callable_names() const noexcept { return callable_order_; }
    Result invoke(std::string_view name);
    Result invoke_event(std::string_view base, std::string_view suffix);
    void remove_callable(std::string_view name);
    void clear_callables(); // 0040cc20: removes scripts AND hosts, raises the clear signal.
    void reset_stack();     // 0040c720: resets stack and acknowledges registry clear.

    // Host callbacks use tag-0 values only (0040c970/0040c990); these methods consume
    // one payload/tag pair. Word values preserve bits; no int/float numeric conversion.
    std::uint32_t pop_word();
    std::int32_t pop_int();
    float pop_float();
    std::string pop_string(); // Copies to native ownership and releases the VM allocation.
    void push_word(std::uint32_t value);
    void push_int(std::int32_t value);
    void push_float(float value);
    std::uint32_t push_string(std::string_view value);
    std::size_t stack_size() const noexcept;

    // Explicit ownership for original saved-variable raw string handles. Retaining
    // shares the same 32-bit address; consuming host arguments release one owner.
    std::string string_at(std::uint32_t address) const;
    std::uint32_t allocate_string(std::string_view value);
    std::uint32_t duplicate_string(std::uint32_t address);
    void retain_string(std::uint32_t address);
    void release_string(std::uint32_t address);

    // Portable checked 32-bit VM addresses, never truncated native pointers.
    std::uint32_t read_word(std::uint32_t address) const;
    void write_word(std::uint32_t address, std::uint32_t value);
    std::uint32_t data_address(std::string_view function) const;
    std::uint32_t data_word(std::string_view function, std::size_t index) const;

private:
    struct Region;
    struct Callable;
    struct StackValue { std::uint32_t payload; std::uint32_t tag; };
    std::map<std::string, std::shared_ptr<Callable>, std::less<>> callables_;
    std::vector<std::string_view> callable_order_;
    std::map<std::uint32_t, std::shared_ptr<Region>> regions_;
    std::array<StackValue, stack_capacity> stack_{};
    std::size_t stack_size_{};
    std::uint32_t next_address_{0x10000};
    bool clear_signal_{};
    std::size_t invocation_depth_{};
    bool cleanup_requested_{};

    std::shared_ptr<Region> allocate_region(std::size_t byte_count, bool string);
    const Region& region_at(std::uint32_t address, std::size_t bytes) const;
    Region& region_at(std::uint32_t address, std::size_t bytes);
    void push(std::uint32_t payload, std::uint32_t tag);
    StackValue pop();
    std::uint32_t resolve(const Callable& function);
    Result execute(const std::shared_ptr<Callable>& function);
    void load_records(const Program& program, const IncludeLoader& includes,
                      std::vector<std::string>& include_stack);
    void collect_data_regions();
    void reclaim_address_tail();
};

} // namespace yami::script
