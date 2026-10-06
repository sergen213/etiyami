#include "script_vm.hpp"

// The check remains runnable when integrated into a Release/NDEBUG build.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

// One framework-free check: synthetic PCS is an opcode oracle, not invented gameplay.
// Shipped scripts are decoded/linked only; actual game calls require the genuine host.
namespace {
using namespace yami::script;
std::uint32_t bits(float value) {
    std::uint32_t word;
    std::memcpy(&word, &value, 4);
    return word;
}
void word(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void name(std::vector<std::uint8_t>& bytes, const std::string& value) {
    word(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
struct Code {
    std::vector<std::uint32_t> words;
    void op(Opcode opcode) { words.push_back(static_cast<std::uint32_t>(opcode)); }
    void operand(Opcode opcode, std::uint32_t value) { op(opcode); words.push_back(value); }
    void text(Opcode opcode, const std::string& value) {
        op(opcode);
        const auto count = (value.size() + 4) / 4;
        words.push_back(static_cast<std::uint32_t>(count));
        const auto start = words.size();
        words.resize(start + count, 0xa5a5a5a5); // Real PCS padding is nonzero residue.
        for (std::size_t i = 0; i <= value.size(); ++i) {
            auto& destination = words[start + i / 4];
            const auto shift = (i % 4) * 8;
            const auto byte = i == value.size() ? 0 : static_cast<unsigned char>(value[i]);
            destination = (destination & ~(std::uint32_t(255) << shift)) | (std::uint32_t(byte) << shift);
        }
    }
    void take() { text(Opcode::Call, "take"); }
    std::size_t jump(Opcode opcode) {
        operand(opcode, 0);
        return words.size() - 1;
    }
    void target(std::size_t operand_index) { words[operand_index] = static_cast<std::uint32_t>(words.size()); }
};
void function(std::vector<std::uint8_t>& bytes, const std::string& function_name,
              const std::vector<std::uint32_t>& data, const Code& code) {
    word(bytes, 0);
    name(bytes, function_name);
    word(bytes, static_cast<std::uint32_t>(data.size()));
    for (const auto value : data) word(bytes, value);
    word(bytes, static_cast<std::uint32_t>(code.words.size()));
    for (const auto value : code.words) word(bytes, value);
}
void include(std::vector<std::uint8_t>& bytes, const std::string& include_name) {
    word(bytes, 1);
    name(bytes, include_name);
}
template<class F> void rejects(F&& action) {
    bool rejected = false;
    try { action(); } catch (const Error&) { rejected = true; }
    assert(rejected);
}

void pure_vm_check() {
    Runtime vm;
    std::vector<std::uint32_t> actual, expected;
    assert(vm.register_host("take", [&](Runtime& runtime) { actual.push_back(runtime.pop_word()); }));
    assert(!vm.register_host("take", [&](Runtime&) { assert(false); }));
    assert(vm.register_host("take_text", [](Runtime& runtime) {
        assert(runtime.pop_string() == std::string("raw_\xfc", 5));
    }));
    assert(vm.register_host("take_address", [](Runtime& runtime) {
        assert(runtime.pop_word() == runtime.data_address("ops"));
    }));
    Code code;
    auto value = [&](std::uint32_t v) { code.operand(Opcode::PushWord, v); };
    auto real = [&](float v) { code.operand(Opcode::PushFloat, bits(v)); };
    auto take = [&](std::uint32_t v) { code.take(); expected.push_back(v); };
    const std::array<std::pair<Opcode,std::uint32_t>,12> integers{{
        {Opcode::AddInt,9},{Opcode::MultiplyInt,18},{Opcode::SubtractInt,3},{Opcode::DivideInt,2},
        {Opcode::LessInt,0},{Opcode::LessEqualInt,0},{Opcode::EqualInt,0},{Opcode::NotEqualInt,1},
        {Opcode::GreaterEqualInt,1},{Opcode::GreaterInt,1},{Opcode::And,2},{Opcode::Or,7}
    }};
    for (const auto& entry : integers) { value(3); value(6); code.op(entry.first); take(entry.second); }
    const std::array<std::pair<Opcode,float>,4> floats{{
        {Opcode::AddFloat,9},{Opcode::MultiplyFloat,18},{Opcode::SubtractFloat,3},{Opcode::DivideFloat,2}
    }};
    for (const auto& entry : floats) { real(3); real(6); code.op(entry.first); take(bits(entry.second)); }
    const std::array<std::pair<Opcode,std::uint32_t>,6> comparisons{{
        {Opcode::LessFloat,0},{Opcode::LessEqualFloat,0},{Opcode::EqualFloat,0},
        {Opcode::NotEqualFloat,1},{Opcode::GreaterEqualFloat,1},{Opcode::GreaterFloat,1}
    }};
    for (const auto& entry : comparisons) { real(3); real(6); code.op(entry.first); take(entry.second); }
    // Arithmetic wraps raw32, comparisons are signed, IDIV truncates toward zero.
    value(0xffffffffu); value(1); code.op(Opcode::AddInt); take(0);
    value(2); value(0xffffffffu); code.op(Opcode::LessInt); take(1);
    value(0xfffffffbu); value(7); code.op(Opcode::DivideInt); take(0xffffffffu);
    value(7); code.op(Opcode::NegateInt); take(0xfffffff9u);
    real(7); code.op(Opcode::NegateFloat); take(bits(-7));
    value(0xfffffff9u); code.op(Opcode::IntToFloat); take(bits(-7));
    real(-7.75f); code.op(Opcode::FloatToInt); take(0xfffffff9u);
    real(0x1p32f); code.op(Opcode::FloatToInt); take(0);
    code.operand(Opcode::PushFloat,0x7f800001u); code.op(Opcode::ResolveFloat); take(0x7fc00001u);
    real(1); code.operand(Opcode::PushFloat,0x7fc00001u); code.op(Opcode::NotEqualFloat); take(1);
    code.operand(Opcode::PushFloat,0x7fc00123u); code.operand(Opcode::PushFloat,0xffc00456u);
    code.op(Opcode::AddFloat); take(0xffc00456u); // x87 NaN payload selection, not host SSE/ARM's.
    code.operand(Opcode::PushFloat,0xffc00123u); code.operand(Opcode::PushFloat,0x7fc00123u);
    code.op(Opcode::MultiplyFloat); take(0x7fc00123u); // Positive sign wins identical payload.
    real(0); real(0); code.op(Opcode::DivideFloat); take(0xffc00000u);
    real(0); code.op(Opcode::NegateFloat); take(0x80000000u);
    // Slot 1 points to slot 0. Direct/indirect and word/float stores share raw memory.
    value(9); code.operand(Opcode::PushLocal,0); code.op(Opcode::StoreWord);
    code.operand(Opcode::PushLocal,0); code.op(Opcode::ResolveWord); take(9);
    code.operand(Opcode::PushAddress,0); code.text(Opcode::Call,"take_address");
    code.operand(Opcode::PushAddress,0); code.operand(Opcode::PushLocal,1); code.op(Opcode::StoreWord);
    code.operand(Opcode::PushIndirect,1); code.op(Opcode::ResolveWord); take(9);
    value(99); code.operand(Opcode::PushIndirect,1); code.op(Opcode::StoreWord);
    code.operand(Opcode::PushLocal,0); code.op(Opcode::ResolveWord); take(99);
    real(12.5f); code.operand(Opcode::PushLocal,2); code.op(Opcode::StoreFloat);
    code.operand(Opcode::PushLocal,2); code.op(Opcode::ResolveFloat); take(bits(12.5f));
    real(4.5f); code.operand(Opcode::PushIndirect,1); code.op(Opcode::StoreFloat);
    code.operand(Opcode::PushIndirect,1); code.op(Opcode::ResolveFloat); take(bits(4.5f));
    code.text(Opcode::PushString,std::string("raw_\xfc",5)); code.text(Opcode::Call,"take_text");
    for (const auto branch : {Opcode::JumpIfZero,Opcode::JumpIfNonzero,Opcode::Jump}) {
        if (branch != Opcode::Jump) value(branch == Opcode::JumpIfZero ? 0 : 1);
        const auto destination = code.jump(branch);
        value(666); code.take(); // Must be skipped, and would fail the result sequence.
        code.target(destination);
        value(12); take(12);
    }
    std::vector<std::uint8_t> bytes;
    function(bytes,"ops",{0,0,0},code);
    Code leave; leave.operand(Opcode::PushWord,42);
    function(bytes,"leave",{},leave);
    Code consume; consume.take(); function(bytes,"consume",{},consume);
    Code increment;
    increment.operand(Opcode::PushWord,1); increment.operand(Opcode::PushLocal,0); increment.op(Opcode::AddInt);
    increment.operand(Opcode::PushLocal,0); increment.op(Opcode::StoreWord);
    increment.operand(Opcode::PushLocal,0); increment.op(Opcode::ResolveWord); increment.take();
    function(bytes,"bump_on_end",{0},increment);
    Code loop;
    loop.operand(Opcode::PushWord,1); loop.operand(Opcode::PushLocal,0); loop.op(Opcode::SubtractInt);
    loop.operand(Opcode::PushLocal,0); loop.op(Opcode::StoreWord);
    loop.operand(Opcode::PushLocal,0); loop.operand(Opcode::JumpIfNonzero,0);
    loop.operand(Opcode::PushLocal,0); loop.op(Opcode::ResolveWord); loop.take();
    function(bytes,"loop",{3},loop);
    // Duplicate function must not replace leave's original instruction block.
    Code duplicate; duplicate.operand(Opcode::PushWord,777);
    function(bytes,"leave",{},duplicate);
    const auto program = Program::decode(bytes,"opcode oracle");
    std::set<Opcode> covered;
    for (const auto& record : program.records)
        for (const auto& instruction : record.function.instructions) covered.insert(instruction.opcode);
    assert(covered.size() == 40);
    vm.load(program);
    assert(vm.invoke("ops") == Runtime::Result::Completed);
    assert(actual == expected && vm.stack_size() == 0);
    assert(vm.data_word("ops",0) == bits(4.5f));
    assert(vm.invoke("leave") == Runtime::Result::Completed && vm.stack_size() == 1);
    vm.invoke("consume"); expected.push_back(42);
    vm.invoke_event("bump","_on_end"); expected.push_back(1);
    vm.invoke_event("bump","_on_end"); expected.push_back(2);
    vm.invoke("loop"); expected.push_back(0);
    assert(actual == expected && vm.stack_size() == 0 && vm.data_word("bump_on_end",0) == 2);
    assert(vm.invoke("absent_event") == Runtime::Result::Missing);
    const auto text = vm.allocate_string("owned");
    const auto copy = vm.duplicate_string(text);
    assert(text != copy && vm.string_at(text) == vm.string_at(copy));
    vm.release_string(text); rejects([&] { vm.string_at(text); }); vm.release_string(copy);
    // A saved string survives consuming a getter result without changing its raw address.
    const auto shared = vm.allocate_string("checkpoint text");
    vm.retain_string(shared);
    vm.push_word(shared);
    assert(vm.pop_string() == "checkpoint text");
    assert(vm.string_at(shared) == "checkpoint text");
    vm.release_string(shared);
    rejects([&] { vm.string_at(shared); });
    // Byte-addressed x86 memory can load/store an unaligned word across adjacent slots.
    const auto address = vm.data_address("ops");
    vm.write_word(address + 1,0x12345678);
    assert(vm.read_word(address + 1) == 0x12345678);
    rejects([&] { vm.read_word(address + 9); });
    rejects([&] { vm.pop_word(); });
    for (std::size_t i = 0; i < Runtime::stack_capacity; ++i) vm.push_word(0);
    rejects([&] { vm.push_word(0); }); vm.reset_stack();
    Code divide; divide.operand(Opcode::PushWord,0); divide.operand(Opcode::PushWord,1); divide.op(Opcode::DivideInt);
    std::vector<std::uint8_t> fault;
    function(fault,"divide_fault",{},divide); vm.load(Program::decode(fault));
    rejects([&] { vm.invoke("divide_fault"); });
    Code clear; clear.text(Opcode::Call,"clear"); clear.operand(Opcode::PushWord,999);
    std::vector<std::uint8_t> clearing;
    function(clearing,"clear_caller",{},clear); vm.load(Program::decode(clearing));
    assert(vm.register_host("clear",[](Runtime& runtime) { runtime.clear_callables(); }));
    assert(vm.invoke("clear_caller") == Runtime::Result::RegistryCleared);
    assert(vm.stack_size() == 0 && !vm.contains("take") && !vm.contains("ops"));
    // New Game clears outside a script call; host bootstrap must acknowledge
    // that signal before the reloaded script's first nested call.
    vm.clear_callables();
    vm.reset_stack();
    vm.register_host("take", [&](Runtime& runtime) { actual.push_back(runtime.pop_word()); });
    vm.load(program);
    const auto previous_results = actual.size();
    assert(vm.invoke("loop") == Runtime::Result::Completed);
    assert(actual.size() == previous_results+1 && actual.back() == 0 && vm.stack_size() == 0);

    // Includes are inserted at the record position and share the original first-wins map.
    Runtime included;
    std::vector<std::uint8_t> including;
    include(including,"child"); function(including,"leave",{},duplicate);
    std::vector<std::uint8_t> child; function(child,"leave",{},leave);
    const auto child_program = Program::decode(child);
    included.load(Program::decode(including),[&](const std::string& dependency) {
        assert(dependency == "child"); return child_program;
    });
    included.invoke("leave"); assert(included.pop_word() == 42);
    std::vector<std::uint8_t> cycle; include(cycle,"cycle");
    const auto cycle_program = Program::decode(cycle);
    rejects([&] { included.load(cycle_program,[&](const std::string&) { return cycle_program; }); });
}

void malformed_check() {
    Code code; code.operand(Opcode::PushWord,17);
    std::vector<std::uint8_t> valid; function(valid,"f",{},code);
    for (std::size_t size = 1; size < valid.size(); ++size) {
        std::vector<std::uint8_t> truncated(valid.begin(),valid.begin() + static_cast<std::ptrdiff_t>(size));
        rejects([&] { Program::decode(truncated); });
    }
    auto bad_tag = valid; bad_tag[0] = 2; rejects([&] { Program::decode(bad_tag); });
    auto bad_count = valid;
    // Layout: tag4, name-count4, name1, data-count4, code-count4, code8.
    bad_count[13] = 255; rejects([&] { Program::decode(bad_count); });
    auto reject_code = [&](const Code& invalid) {
        std::vector<std::uint8_t> bytes; function(bytes,"f",{},invalid);
        rejects([&] { Program::decode(bytes); });
    };
    Code unknown; unknown.words = {40}; reject_code(unknown);
    Code missing; missing.op(Opcode::PushWord); reject_code(missing);
    Code unterminated; unterminated.words = {5,1,0x41414141}; reject_code(unterminated);
    Code mid_operand; mid_operand.words = {30,1}; reject_code(mid_operand);
    Code bad_data; bad_data.operand(Opcode::PushLocal,0); reject_code(bad_data);
    std::vector<std::uint8_t> escaping; include(escaping,"../escape");
    rejects([&] { Program::decode(escaping); });
    std::vector<std::uint8_t> too_long; function(too_long,std::string(64,'a'),{},Code{});
    rejects([&] { Program::decode(too_long); });
}

void shipped_check(const std::filesystem::path& directory) {
    constexpr std::array<std::size_t,4> counts{{33,42,51,3}};
    constexpr std::array<std::size_t,4> word_counts{{1621,1243,2308,117}};
    constexpr std::array<std::size_t,4> instruction_counts{{360,299,476,31}};
    const std::set<Opcode> shipped_opcodes{Opcode::PushWord,Opcode::PushFloat,Opcode::PushString,
        Opcode::EqualInt,Opcode::JumpIfZero,Opcode::Jump,Opcode::Call};
    for (std::size_t level = 0; level < 4; ++level) {
        const auto path = directory / ("level" + std::to_string(level + 1) + ".pcs");
        const auto program = Program::read_file(path);
        assert(program.records.size() == counts[level]);
        std::set<std::string> names;
        for (const auto& spec : game_host_specs()) names.insert(spec.name);
        for (const auto& record : program.records) names.insert(record.name);
        std::size_t total_words = 0, total_instructions = 0;
        for (const auto& record : program.records) {
            assert(!record.include && record.function.initial_data.empty());
            total_words += record.function.code_word_count;
            total_instructions += record.function.instructions.size();
            for (const auto& instruction : record.function.instructions) {
                assert(shipped_opcodes.count(instruction.opcode) == 1);
                assert(opcode_address(instruction.opcode) != 0);
                if (instruction.opcode == Opcode::Call) assert(names.count(instruction.text) == 1);
            }
        }
        assert(total_words == word_counts[level] && total_instructions == instruction_counts[level]);
        Runtime registry;
        registry.load(program); // No game execution or fabricated host handlers.
        for (const auto& record : program.records) assert(registry.contains(record.name));
        if (level == 2) {
            std::size_t duplicates = 0;
            for (const auto& record : program.records)
                duplicates += record.name == "alet_kutulu_robot_piyon_5_on_dead";
            assert(duplicates == 2);
        }
        std::cout << path.filename().string() << ": " << counts[level] << " records, "
                  << total_words << " code words, " << total_instructions << " decoded instructions\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        pure_vm_check();
        malformed_check();
        shipped_check(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("game/data/scripts"));
        std::cout << "All 40 VM opcodes, shared-stack/reference ownership and PCS boundaries checked\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
