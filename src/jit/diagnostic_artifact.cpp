#include "jit/diagnostic_artifact.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCTargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace atelier::source::detail {
namespace {

template <class T>
T take(llvm::Expected<T> value) {
    if (!value)
        throw std::runtime_error("JIT object disassembly: " + llvm::toString(value.takeError()));
    return std::move(*value);
}

struct CodeSymbol {
    std::uint64_t section_index;

    std::uint64_t offset;

    llvm::StringRef name;
};

} // namespace

std::string disassemble_object(llvm::MemoryBufferRef object) {
    auto file = take(llvm::object::ObjectFile::createObjectFile(object));
    if (!file->isCOFF() || !file->isRelocatableObject() || file->getArch() != llvm::Triple::x86_64)
        throw std::runtime_error("JIT object disassembly requires a relocatable x64 COFF object.");

    static const bool initialization_failed = llvm::InitializeNativeTargetDisassembler();
    if (initialization_failed)
        throw std::runtime_error("LLVM native disassembler initialization failed.");

    const auto triple = file->makeTriple();
    std::string error;
    const auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target)
        throw std::runtime_error("JIT object disassembly: " + error);

    std::unique_ptr<llvm::MCRegisterInfo> registers(target->createMCRegInfo(triple));
    std::unique_ptr<llvm::MCInstrInfo> instructions(target->createMCInstrInfo());
    std::unique_ptr<llvm::MCSubtargetInfo> subtarget(target->createMCSubtargetInfo(triple, "", ""));
    if (!registers || !instructions || !subtarget)
        throw std::runtime_error("LLVM x64 disassembler metadata is unavailable.");
    std::unique_ptr<llvm::MCAsmInfo> assembly(target->createMCAsmInfo(*registers, triple, llvm::MCTargetOptions{}));
    if (!assembly)
        throw std::runtime_error("LLVM x64 assembly metadata is unavailable.");

    llvm::MCContext context(triple, *assembly, *registers, *subtarget);
    std::unique_ptr<llvm::MCDisassembler> disassembler(target->createMCDisassembler(*subtarget, context));
    std::unique_ptr<llvm::MCInstPrinter> printer(
        target->createMCInstPrinter(triple, 1, *assembly, *instructions, *registers));
    if (!disassembler || !printer)
        throw std::runtime_error("LLVM x64 disassembler or Intel instruction printer is unavailable.");
    printer->setPrintImmHex(true);
    printer->setPrintBranchImmAsAddress(true);

    std::vector<CodeSymbol> symbols;
    for (const auto& symbol : file->symbols()) {
        if (take(symbol.getType()) != llvm::object::SymbolRef::ST_Function)
            continue;
        const auto section = take(symbol.getSection());
        if (section == file->section_end() || !section->isText())
            continue;
        const auto name = take(symbol.getName());
        if (name.empty())
            continue;
        // A COFF symbol's value is relative to its defining section.
        const auto offset = take(symbol.getValue());
        if (offset > section->getSize())
            throw std::runtime_error("JIT object function symbol lies outside its code section.");
        symbols.push_back({section->getIndex(), offset, name});
    }
    std::sort(symbols.begin(), symbols.end(), [](const CodeSymbol& left, const CodeSymbol& right) {
        if (left.section_index != right.section_index)
            return left.section_index < right.section_index;
        if (left.offset != right.offset)
            return left.offset < right.offset;
        return left.name < right.name;
    });

    std::string result;
    llvm::raw_string_ostream output(result);
    output << "; x64 COFF object disassembly, Intel syntax, before linking\n"
              "; Addresses and branch targets are section-relative offsets, not runtime addresses.\n"
              "; Operands covered by the listed relocations remain unresolved until linking.\n";
    bool decoded_instruction = false;
    for (const auto& section : file->sections()) {
        if (!section.isText())
            continue;
        const auto contents = take(section.getContents());
        if (contents.empty())
            continue;
        const auto name = take(section.getName());
        const auto section_index = section.getIndex();
        const llvm::ArrayRef<std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(contents.data()),
                                                 contents.size());
        output << "\nSection " << name << " (#" << section_index << ", " << bytes.size() << " bytes):\n";
        auto symbol = std::lower_bound(
            symbols.begin(), symbols.end(), section_index,
            [](const CodeSymbol& candidate, std::uint64_t index) { return candidate.section_index < index; });
        const auto print_symbols = [&](std::uint64_t offset) {
            while (symbol != symbols.end() && symbol->section_index == section_index && symbol->offset <= offset) {
                output << '\n' << llvm::format_hex(symbol->offset, 10) << " <" << symbol->name << ">:\n";
                ++symbol;
            }
        };

        for (std::uint64_t offset = 0; offset < bytes.size();) {
            print_symbols(offset);
            llvm::MCInst instruction;
            std::uint64_t size = 0;
            const auto status =
                disassembler->getInstruction(instruction, size, bytes.drop_front(offset), offset, llvm::nulls());
            if (status == llvm::MCDisassembler::Fail || !size || size > bytes.size() - offset)
                throw std::runtime_error("Could not decode JIT object section " + name.str() + " at byte offset " +
                                         std::to_string(offset) + ".");
            output << "  " << llvm::format_hex(offset, 10) << ": ";
            for (std::uint64_t index = 0; index < size; ++index)
                output << llvm::format_hex_no_prefix(bytes[offset + index], 2) << ' ';
            if (size < 15)
                output.indent(static_cast<unsigned>((15 - size) * 3));
            // x86 PC-relative operands use the end of the instruction as their base.
            printer->printInst(&instruction, offset + size, "", *subtarget, output);
            if (status == llvm::MCDisassembler::SoftFail)
                output << " ; invalid instruction encoding";
            output << '\n';
            decoded_instruction = true;
            offset += size;
        }
        print_symbols(bytes.size());

        bool has_relocations = false;
        for (const auto& relocation : section.relocations()) {
            if (!has_relocations)
                output << "\n  Relocations (section-relative offsets):\n";
            has_relocations = true;
            llvm::SmallString<32> type;
            relocation.getTypeName(type);
            output << "    +" << llvm::format_hex(relocation.getOffset(), 10) << ' ' << type << ' ';
            const auto target_symbol = relocation.getSymbol();
            if (target_symbol == file->symbol_end())
                output << "<no symbol>";
            else
                output << take(target_symbol->getName());
            output << '\n';
        }
    }
    if (!decoded_instruction)
        throw std::runtime_error("JIT object disassembly found no instructions in code sections.");
    return result;
}

} // namespace atelier::source::detail
