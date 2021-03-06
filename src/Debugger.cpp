#include "Debugger.h"
#include "CircularBuffer.h"
#include "ConsoleOutput.h"
#include "Cpu.h"
#include "CpuHelpers.h"
#include "CpuOpCodes.h"
#include "ErrorHandler.h"
#include "MemoryBus.h"
#include "Platform.h"
#include "RegexHelpers.h"
#include "Stream.h"
#include "StringHelpers.h"
#include "SyncProtocol.h"
#include "Via.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {
    struct ScopedConsoleCtrlHandler {
        template <typename Handler>
        ScopedConsoleCtrlHandler(Handler handler) {
            m_oldHandler = Platform::GetConsoleCtrlHandler();
            Platform::SetConsoleCtrlHandler(handler);
        }

        ~ScopedConsoleCtrlHandler() { Platform::SetConsoleCtrlHandler(m_oldHandler); }

    private:
        decltype(Platform::GetConsoleCtrlHandler()) m_oldHandler;
    };

    template <typename T>
    T HexStringToIntegral(const char* s) {
        std::stringstream converter(s);
        int64_t value;
        converter >> std::hex >> value;
        return static_cast<T>(value);
    }

    template <typename T>
    T HexStringToIntegral(const std::string& s) {
        return HexStringToIntegral<T>(s.c_str());
    }

    template <typename T>
    T StringToIntegral(std::string s) {
        if (s.length() == 0)
            return 0;

        if (s[0] == '$') { // '$' Hex value
            return HexStringToIntegral<T>(s.substr(1));
        } else if (s[0] == '0' && s[1] == 'x' || s[1] == 'X') { // '0x' Hex value
            return HexStringToIntegral<T>(s);
        } else { // Integral value
            int64_t value;
            std::stringstream converter(s);
            converter >> value;
            return static_cast<T>(value);
        }
    }

    std::vector<std::string> Tokenize(const std::string& s) { return Split(s, " \t"); }

    std::string TryMemoryBusRead(const MemoryBus& memoryBus, uint16_t address) {
        try {
            uint8_t value = memoryBus.Read(address);
            return FormattedString<>("$%02x (%d)", value, value).Value();
        } catch (...) {
            return "INVALID_READ";
        }
    }

    const char* GetRegisterName(const CpuRegisters& cpuRegisters, const uint8_t& r) {
        ptrdiff_t offset =
            reinterpret_cast<const uint8_t*>(&r) - reinterpret_cast<const uint8_t*>(&cpuRegisters);
        switch (offset) {
        case offsetof(CpuRegisters, A):
            return "A";
        case offsetof(CpuRegisters, B):
            return "B";
        case offsetof(CpuRegisters, DP):
            return "DP";
        case offsetof(CpuRegisters, CC):
            return "CC";
        default:
            FAIL();
            return "INVALID";
        }
    };

    const char* GetRegisterName(const CpuRegisters& cpuRegisters, const uint16_t& r) {
        ptrdiff_t offset =
            reinterpret_cast<const uint8_t*>(&r) - reinterpret_cast<const uint8_t*>(&cpuRegisters);
        switch (offset) {
        case offsetof(CpuRegisters, X):
            return "X";
        case offsetof(CpuRegisters, Y):
            return "Y";
        case offsetof(CpuRegisters, U):
            return "U";
        case offsetof(CpuRegisters, S):
            return "S";
        case offsetof(CpuRegisters, PC):
            return "PC";
        case offsetof(CpuRegisters, D):
            return "D";
        default:
            FAIL();
            return "INVALID";
        }
    };

    struct Instruction {
        const CpuOp* cpuOp;
        int page;
        std::array<uint8_t, 5> opBytes; // Max 2 byte opcode + 3 byte operands
        size_t firstOperandIndex = 0;

        uint8_t GetOperand(size_t index) const { return opBytes[firstOperandIndex + index]; }
    };

    struct InstructionTraceInfo {
        Instruction instruction{};
        CpuRegisters preOpCpuRegisters;
        CpuRegisters postOpCpuRegisters{};
        cycles_t elapsedCycles{};

        static const size_t MaxMemoryAccesses = 16;
        struct MemoryAccess {
            uint16_t address{};
            uint16_t value{};
            bool read{};
        };
        std::array<MemoryAccess, MaxMemoryAccesses> memoryAccesses;
        size_t numMemoryAccesses = 0;

        void AddMemoryAccess(uint16_t address, uint16_t value, bool read) {
            assert(numMemoryAccesses < memoryAccesses.size());
            memoryAccesses[numMemoryAccesses++] = {address, value, read};
        }
    };

    Instruction ReadInstruction(uint16_t opAddr, const MemoryBus& memoryBus) {
        Instruction instruction{};

        // Always read max opBytes size even if not all the bytes are for this instruction. We can't
        // really know up front how many bytes an op will take because indexed instructions
        // sometimes read an extra operand byte (determined dynamically).
        for (auto& byte : instruction.opBytes)
            byte = memoryBus.Read(opAddr++);

        int cpuOpPage = 0;
        size_t opCodeIndex = 0;
        if (IsOpCodePage1(instruction.opBytes[opCodeIndex])) {
            cpuOpPage = 1;
            ++opCodeIndex;
        } else if (IsOpCodePage2(instruction.opBytes[opCodeIndex])) {
            cpuOpPage = 2;
            ++opCodeIndex;
        }

        instruction.cpuOp = &LookupCpuOpRuntime(cpuOpPage, instruction.opBytes[opCodeIndex]);
        instruction.page = cpuOpPage;
        instruction.firstOperandIndex = opCodeIndex + 1;
        return instruction;
    }

    void PreOpWriteTraceInfo(InstructionTraceInfo& traceInfo, const CpuRegisters& cpuRegisters,
                             /*const*/ MemoryBus& memoryBus) {
        memoryBus.SetCallbacksEnabled(false);
        traceInfo.instruction = ReadInstruction(cpuRegisters.PC, memoryBus);
        traceInfo.preOpCpuRegisters = cpuRegisters;
        memoryBus.SetCallbacksEnabled(true);
    }

    void PostOpWriteTraceInfo(InstructionTraceInfo& traceInfo, const CpuRegisters& cpuRegisters,
                              cycles_t elapsedCycles) {
        traceInfo.postOpCpuRegisters = cpuRegisters;
        traceInfo.elapsedCycles = elapsedCycles;
    }

    void DisassembleOp_EXG_TFR(const Instruction& instruction, const CpuRegisters& cpuRegisters,
                               std::string& disasmInstruction, std::string& comment) {
        (void)cpuRegisters;
        (void)comment;

        const auto& cpuOp = instruction.cpuOp;
        ASSERT(cpuOp->addrMode == AddressingMode::Inherent);
        uint8_t postbyte = instruction.GetOperand(0);
        uint8_t src = (postbyte >> 4) & 0b111;
        uint8_t dst = postbyte & 0b111;
        if (postbyte & BITS(3)) {
            const char* const regName[]{"A", "B", "CC", "DP"};
            disasmInstruction =
                FormattedString<>("%s %s,%s", cpuOp->name, regName[src], regName[dst]);
        } else {
            const char* const regName[]{"D", "X", "Y", "U", "S", "PC"};
            disasmInstruction =
                FormattedString<>("%s %s,%s", cpuOp->name, regName[src], regName[dst]);
        }
    }

    void DisassembleOp_PSH_PUL(const Instruction& instruction, const CpuRegisters& cpuRegisters,
                               std::string& disasmInstruction, std::string& comment) {
        (void)cpuRegisters;

        const auto& cpuOp = instruction.cpuOp;
        ASSERT(cpuOp->addrMode == AddressingMode::Immediate);
        auto value = instruction.GetOperand(0);
        std::vector<std::string> registers;
        if (value & BITS(0))
            registers.push_back("CC");
        if (value & BITS(1))
            registers.push_back("A");
        if (value & BITS(2))
            registers.push_back("B");
        if (value & BITS(3))
            registers.push_back("DP");
        if (value & BITS(4))
            registers.push_back("X");
        if (value & BITS(5))
            registers.push_back("Y");
        if (value & BITS(6)) {
            registers.push_back(cpuOp->opCode < 0x36 ? "U" : "S");
        }
        if (value & BITS(7))
            registers.push_back("PC");

        disasmInstruction = FormattedString<>("%s %s", cpuOp->name, Join(registers, ",").c_str());
        comment = FormattedString<>("#$%02x (%d)", value, value);
    }

    void DisassembleIndexedInstruction(const Instruction& instruction,
                                       const CpuRegisters& cpuRegisters,
                                       std::string& disasmInstruction, std::string& comment) {
        auto RegisterSelect = [&cpuRegisters](uint8_t postbyte) -> const uint16_t& {
            switch ((postbyte >> 5) & 0b11) {
            case 0b00:
                return cpuRegisters.X;
            case 0b01:
                return cpuRegisters.Y;
            case 0b10:
                return cpuRegisters.U;
            default: // 0b11:
                return cpuRegisters.S;
            }
        };

        uint16_t EA = 0;
        uint8_t postbyte = instruction.GetOperand(0);
        bool supportsIndirect = true;
        std::string operands;

        if ((postbyte & BITS(7)) == 0) // (+/- 4 bit offset),R
        {
            // postbyte is a 5 bit two's complement number we convert to 8 bit.
            // So if bit 4 is set (sign bit), we extend the sign bit by turning on bits 6,7,8;
            int8_t offset = postbyte & 0b0001'1111;
            if (postbyte & BITS(4))
                offset |= 0b1110'0000;
            auto& reg = RegisterSelect(postbyte);
            EA = reg + offset;
            supportsIndirect = false;

            operands = FormattedString<>("%d,%s", offset, GetRegisterName(cpuRegisters, reg));
            comment = FormattedString<>("%d,$%04x", offset, reg);
        } else {
            switch (postbyte & 0b1111) {
            case 0b0000: { // ,R+
                auto& reg = RegisterSelect(postbyte);
                EA = reg;
                supportsIndirect = false;

                operands = FormattedString<>(",%s+", GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>(",$%04x+", reg);
            } break;
            case 0b0001: { // ,R++
                auto& reg = RegisterSelect(postbyte);
                EA = reg;

                operands = FormattedString<>(",%s++", GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>(",$%04x++", reg);
            } break;
            case 0b0010: { // ,-R
                auto& reg = RegisterSelect(postbyte);
                EA = reg - 1;
                supportsIndirect = false;

                operands = FormattedString<>(",-%s", GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>(",-$%04x", reg);
            } break;
            case 0b0011: { // ,--R
                auto& reg = RegisterSelect(postbyte);
                EA = reg - 2;

                operands = FormattedString<>(",--%s", GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>(",--$%04x", reg);
            } break;
            case 0b0100: { // ,R
                auto& reg = RegisterSelect(postbyte);
                EA = reg;

                operands = FormattedString<>(",%s", GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>(",$%04x", reg);
            } break;
            case 0b0101: { // (+/- B),R
                auto& reg = RegisterSelect(postbyte);
                auto offset = S16(cpuRegisters.B);
                EA = reg + offset;

                operands = FormattedString<>("B,%s", GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>("%d,$%04x", offset, reg);
            } break;
            case 0b0110: { // (+/- A),R
                auto& reg = RegisterSelect(postbyte);
                auto offset = S16(cpuRegisters.A);
                EA = reg + offset;

                operands = FormattedString<>("A,%s", GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>("%d,$%04x", offset, reg);
            } break;
            case 0b0111:
                FAIL_MSG("Illegal");
                break;
            case 0b1000: { // (+/- 7 bit offset),R
                auto& reg = RegisterSelect(postbyte);
                uint8_t postbyte2 = instruction.GetOperand(1);
                auto offset = S16(postbyte2);
                EA = reg + offset;

                operands = FormattedString<>("%d,%s", offset, GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>("%d,$%04x", offset, reg);
            } break;
            case 0b1001: { // (+/- 15 bit offset),R
                uint8_t postbyte2 = instruction.GetOperand(1);
                uint8_t postbyte3 = instruction.GetOperand(2);
                auto& reg = RegisterSelect(postbyte);
                auto offset = CombineToS16(postbyte2, postbyte3);
                EA = reg + offset;

                operands = FormattedString<>("%d,%s", offset, GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>("%d,$%04x", offset, reg);
            } break;
            case 0b1010:
                FAIL_MSG("Illegal");
                break;
            case 0b1011: { // (+/- D),R
                auto& reg = RegisterSelect(postbyte);
                auto offset = S16(cpuRegisters.D);
                EA = reg + offset;

                operands = FormattedString<>("D,%s", GetRegisterName(cpuRegisters, reg));
                comment = FormattedString<>("%d,$%04x", offset, reg);
            } break;
            case 0b1100: { // (+/- 7 bit offset),PC
                uint8_t postbyte2 = instruction.GetOperand(1);
                auto offset = S16(postbyte2);
                EA = cpuRegisters.PC + offset;

                operands = FormattedString<>("%d,PC", offset);
                comment = FormattedString<>("%d,$%04x", offset, cpuRegisters.PC);
            } break;
            case 0b1101: { // (+/- 15 bit offset),PC
                uint8_t postbyte2 = instruction.GetOperand(1);
                uint8_t postbyte3 = instruction.GetOperand(2);
                auto offset = CombineToS16(postbyte2, postbyte3);
                EA = cpuRegisters.PC + offset;

                operands = FormattedString<>("%d,PC", offset);
                comment = FormattedString<>("%d,$%04x", offset, cpuRegisters.PC);
            } break;
            case 0b1110:
                FAIL_MSG("Illegal");
                break;
            case 0b1111: { // [address] (Indirect-only)
                uint8_t postbyte2 = instruction.GetOperand(1);
                uint8_t postbyte3 = instruction.GetOperand(2);
                EA = CombineToS16(postbyte2, postbyte3);
            } break;
            default:
                FAIL_MSG("Illegal");
                break;
            }
        }

        if (supportsIndirect && (postbyte & BITS(4))) {
            operands = FormattedString<>("[$%04x]", EA);
        }

        disasmInstruction = std::string(instruction.cpuOp->name) + " " + operands;
    }

    struct DisassembledOp {
        std::string hexInstruction;
        std::string disasmInstruction;
        std::string comment;
        std::string description;
    };

    DisassembledOp DisassembleOp(const InstructionTraceInfo& traceInfo,
                                 const Debugger::SymbolTable& symbolTable) {
        const auto& instruction = traceInfo.instruction;
        const auto& cpuRegisters = traceInfo.preOpCpuRegisters;
        const auto& cpuOp = instruction.cpuOp;

        // Output instruction in hex
        std::string hexInstruction;
        for (uint16_t i = 0; i < cpuOp->size; ++i) {
            hexInstruction += FormattedString<>("%02x", instruction.opBytes[i]);
        }

        std::string disasmInstruction, comment;

        // First see if we have instruction-specific handlers. These are for special cases where the
        // default addressing mode handlers don't give enough information.
        bool handled = true;
        switch (cpuOp->opCode) {
        case 0x1E: // EXG
        case 0x1F: // TFR
            DisassembleOp_EXG_TFR(instruction, cpuRegisters, disasmInstruction, comment);
            break;

        case 0x34: // PSHS
        case 0x35: // PULS
        case 0x36: // PSHU
        case 0x37: // PULU
            DisassembleOp_PSH_PUL(instruction, cpuRegisters, disasmInstruction, comment);
            break;

        default:
            handled = false;
        }

        // If no instruction-specific handler, we disassemble based on addressing mode.
        if (!handled) {
            switch (cpuOp->addrMode) {
            case AddressingMode::Inherent: {
                disasmInstruction = cpuOp->name;
            } break;

            case AddressingMode::Immediate: {
                if (cpuOp->size == 2) {
                    auto value = instruction.GetOperand(0);
                    disasmInstruction = FormattedString<>("%s #$%02x", cpuOp->name, value);
                    comment = FormattedString<>("(%d)", value);
                } else {
                    auto value = CombineToU16(instruction.GetOperand(0), instruction.GetOperand(1));
                    disasmInstruction = FormattedString<>("%s #$%04x", cpuOp->name, value);
                    comment = FormattedString<>("(%d)", value);
                }
            } break;

            case AddressingMode::Extended: {
                auto msb = instruction.GetOperand(0);
                auto lsb = instruction.GetOperand(1);
                uint16_t EA = CombineToU16(msb, lsb);
                disasmInstruction = FormattedString<>("%s $%04x", cpuOp->name, EA);
            } break;

            case AddressingMode::Direct: {
                uint16_t EA = CombineToU16(cpuRegisters.DP, instruction.GetOperand(0));
                disasmInstruction =
                    FormattedString<>("%s $%02x", cpuOp->name, instruction.GetOperand(0));
                comment = FormattedString<>("DP:(PC) = $%02x", EA);
            } break;

            case AddressingMode::Indexed: {
                DisassembleIndexedInstruction(instruction, cpuRegisters, disasmInstruction,
                                              comment);
            } break;

            case AddressingMode::Relative: {
                // Branch instruction with 8 or 16 bit signed relative offset
                uint16_t nextPC = cpuRegisters.PC + cpuOp->size;
                if (cpuOp->size == 2) {
                    auto offset = static_cast<int8_t>(instruction.GetOperand(0));
                    disasmInstruction =
                        FormattedString<>("%s $%02x", cpuOp->name, U16(offset) & 0x00FF);
                    comment =
                        FormattedString<>("(%d), PC + offset = $%04x", offset, nextPC + offset);
                } else {
                    // Could be a long branch from page 0 (3 bytes) or page 1 (4 bytes)
                    ASSERT(cpuOp->size >= 3);
                    auto offset = static_cast<int16_t>(
                        CombineToU16(instruction.GetOperand(0), instruction.GetOperand(1)));
                    disasmInstruction = FormattedString<>("%s $%04x", cpuOp->name, offset);
                    comment =
                        FormattedString<>("(%d), PC + offset = $%04x", offset, nextPC + offset);
                }
            } break;

            case AddressingMode::Illegal: {
            case AddressingMode::Variant:
                FAIL_MSG("Unexpected addressing mode");
            } break;
            }
        }

        // Appends symbol names to known addresses
        auto AppendSymbols = [&symbolTable](const std::string& s) {
            if (!symbolTable.empty()) {
                auto AppendSymbol = [&symbolTable](const std::smatch& m) -> std::string {
                    std::string result = m.str(0);
                    uint16_t address = StringToIntegral<uint16_t>(m.str(0));

                    auto range = symbolTable.equal_range(address);
                    if (range.first != range.second) {
                        std::vector<std::string> symbols;
                        std::transform(range.first, range.second, std::back_inserter(symbols),
                                       [](auto& kvp) { return kvp.second; });

                        result += "{" + Join(symbols, "|") + "}";
                    }
                    return result;
                };

                std::regex re("\\$[A-Fa-f0-9][A-Fa-f0-9][A-Fa-f0-9][A-Fa-f0-9]");
                return RegexReplace(s, re, AppendSymbol);
            }
            return s;
        };

        // Append memory accesses to comment section (if any)
        {
            // Skip the opcode + operand bytes - @TODO: we probably shouldn't be storing these in
            // the first place
            const size_t skipBytes = traceInfo.instruction.cpuOp->size;
            const bool initialSpace = !comment.empty();
            for (size_t i = skipBytes; i < traceInfo.numMemoryAccesses; ++i) {
                auto& ma = traceInfo.memoryAccesses[i];
                const char* separator = i == skipBytes ? (initialSpace ? " " : "") : " ";
                comment += FormattedString<>("%s$%04x%s$%x", separator, ma.address,
                                             ma.read ? "->" : "<-", ma.value);
            }
        }

        disasmInstruction = AppendSymbols(disasmInstruction);
        comment = AppendSymbols(comment);

        return {hexInstruction, disasmInstruction, comment, cpuOp->description};
    };

    std::string GetCCString(const CpuRegisters& cpuRegisters) {
        const auto& cc = cpuRegisters.CC;
        return FormattedString<>("%c%c%c%c%c%c%c%c", cc.Entire ? 'E' : 'e',
                                 cc.FastInterruptMask ? 'F' : 'f', cc.HalfCarry ? 'H' : 'h',
                                 cc.InterruptMask ? 'I' : 'i', cc.Negative ? 'N' : 'n',
                                 cc.Zero ? 'Z' : 'z', cc.Overflow ? 'V' : 'v', cc.Carry ? 'C' : 'c')
            .Value();
    }

    void PrintRegisters(const CpuRegisters& cpuRegisters) {
        const auto& r = cpuRegisters;
        Printf("A=$%02x (%d) B=$%02x (%d) D=$%04x (%d) X=$%04x (%d) "
               "Y=$%04x (%d) U=$%04x S=$%04x DP=$%02x PC=$%04x CC=%s",
               r.A, r.A, r.B, r.B, r.D, r.D, r.X, r.X, r.Y, r.Y, r.U, r.S, r.DP, r.PC,
               GetCCString(cpuRegisters).c_str());
    }

    void PrintRegistersCompact(const CpuRegisters& cpuRegisters) {
        const auto& r = cpuRegisters;
        Printf("A$%02x|B$%02x|X$%04x|Y$%04x|U$%04x|S$%04x|DP$%02x|%s", r.A, r.B, r.X, r.Y, r.U, r.S,
               r.DP, GetCCString(cpuRegisters).c_str());
    }

    void PrintOp(const InstructionTraceInfo& traceInfo, const Debugger::SymbolTable& symbolTable) {
        auto op = DisassembleOp(traceInfo, symbolTable);

        using namespace Platform;
        ScopedConsoleColor scc(ConsoleColor::Gray);
        Printf("[$%04x] ", traceInfo.preOpCpuRegisters.PC);
        SetConsoleColor(ConsoleColor::LightYellow);
        Printf("%-10s ", op.hexInstruction.c_str());
        SetConsoleColor(ConsoleColor::LightAqua);
        Printf("%-32s ", op.disasmInstruction.c_str());
        SetConsoleColor(ConsoleColor::LightGreen);
        Printf("%-40s ", op.comment.c_str());
        SetConsoleColor(ConsoleColor::LightPurple);
        Printf("%2llu ", traceInfo.elapsedCycles);
        PrintRegistersCompact(traceInfo.postOpCpuRegisters);
        Printf("\n");
    }

    void PrintHelp() {
        Printf("s[tep] [count]               step instruction [count] times\n"
               "c[ontinue]                   continue running\n"
               "u[ntil] <address>            run until address is reached\n"
               "info reg[isters]             display register values\n"
               "p[rint] <address>            display value add address\n"
               "set <address>=<value>        set value at address\n"
               "info break                   display breakpoints\n"
               "b[reak] <address>            set instruction breakpoint at address\n"
               "[ |r|a]watch <address>       set write/read/both watchpoint at address\n"
               "delete <index>               delete breakpoint at index\n"
               "disable <index>              disable breakpoint at index\n"
               "enable <index>               enable breakpoint at index\n"
               "loadsymbols <file>           load file with symbol/address definitions\n"
               "toggle ...                   toggle input option\n"
               "  color                        colored output (slow)\n"
               "  trace                        disassembly trace\n"
               "option ...                   set option\n"
               "  errors [ignore|log|fail]     error policy\n"
               "t[race] [...]                display trace output\n"
               "  -n <num_lines>               display num_lines worth\n"
               "  -f <file_name>               output trace to file_name\n"
               "q[uit]                       quit\n"
               "h[elp]                       display this help text\n");
    }

    bool LoadUserSymbolsFile(const char* file, Debugger::SymbolTable& symbolTable) {
        std::ifstream fin(file);
        if (!fin)
            return false;

        std::string line;
        while (std::getline(fin, line)) {
            auto tokens = Tokenize(line);
            if (tokens.size() >= 3 && ((tokens[1].find("EQU") != -1) ||
                                       (tokens[1].find("equ") != -1) || (tokens[1] == ":"))) {
                auto address = StringToIntegral<uint16_t>(tokens[2]);
                symbolTable.insert({address, tokens[0]});
            }
        }
        return true;
    }

    void SetColorEnabled(bool enabled) {
        Platform::SetConsoleColoringEnabled(enabled);
        if (enabled) {
            // For colored trace, we must disable buffering for it to work (slow)
            setvbuf(stdout, NULL, _IONBF, 0);
        } else {
            // With color disabled, we can now buffer output in large chunks
            setvbuf(stdout, NULL, _IOFBF, 100 * 1024);
        }
    }

    inline uint32_t Crc32(uint32_t crc, const void* buffer, size_t len) {
        // CRC-32C (iSCSI) polynomial in reversed bit order.
        const auto POLY = 0x82f63b78;
        // CRC-32 (Ethernet, ZIP, etc.) polynomial in reversed bit order.
        // const auto POLY = 0xedb88320;

        auto buf = reinterpret_cast<const uint8_t*>(buffer);

        crc = ~crc;
        while (len--) {
            crc ^= *buf++;
            for (int k = 0; k < 8; k++)
                crc = crc & 1 ? (crc >> 1) ^ POLY : crc >> 1;
        }
        return ~crc;
    }

    template <typename T>
    uint32_t Crc32(uint32_t crc, const T& value) {
        return Crc32(crc, &value, sizeof(value));
    }

    uint32_t HashInstructionTraceInfo(uint32_t currInstructionHash,
                                      const InstructionTraceInfo& traceInfo) {
        currInstructionHash += Crc32(currInstructionHash, traceInfo.instruction.cpuOp->opCode);
        currInstructionHash += Crc32(currInstructionHash, traceInfo.instruction.cpuOp->addrMode);
        currInstructionHash += Crc32(currInstructionHash, traceInfo.instruction.page);
        currInstructionHash += Crc32(currInstructionHash, traceInfo.elapsedCycles);
        for (size_t i = 0; i < traceInfo.numMemoryAccesses; ++i) {
            currInstructionHash += Crc32(currInstructionHash, traceInfo.memoryAccesses[i].address);
            currInstructionHash += Crc32(currInstructionHash, traceInfo.memoryAccesses[i].read);
            currInstructionHash += Crc32(currInstructionHash, traceInfo.memoryAccesses[i].value);
        }
        currInstructionHash += Crc32(currInstructionHash, traceInfo.preOpCpuRegisters);
        currInstructionHash += Crc32(currInstructionHash, traceInfo.postOpCpuRegisters);
        return currInstructionHash;
    }

    // Global variables
    const size_t MaxTraceInstructions = 1000'000;
    CircularBuffer<InstructionTraceInfo> g_instructionTraceBuffer(MaxTraceInstructions);
    InstructionTraceInfo* g_currTraceInfo = nullptr;

} // namespace

void Debugger::Init(MemoryBus& memoryBus, Cpu& cpu, Via& via) {
    m_memoryBus = &memoryBus;
    m_cpu = &cpu;
    m_via = &via;

    Platform::SetConsoleCtrlHandler([this] {
        BreakIntoDebugger();
        return true;
    });

    SetColorEnabled(m_colorEnabled);

    m_lastCommand = "step"; // Reasonable default

    // Break on start?
    m_breakIntoDebugger = false;

    // Enable trace by default?
    m_traceEnabled = true;

    m_memoryBus->RegisterCallbacks(
        // OnRead
        [&](uint16_t address, uint8_t value) {
            if (m_traceEnabled && g_currTraceInfo) {
                g_currTraceInfo->AddMemoryAccess(address, value, true);
            }

            if (auto bp = m_breakpoints.Get(address)) {
                if (bp->enabled && (bp->type == Breakpoint::Type::Read ||
                                    bp->type == Breakpoint::Type::ReadWrite)) {
                    BreakIntoDebugger();
                    Printf("Watchpoint hit at $%04x (read value $%02x)\n", address, value);
                }
            }
        },
        // OnWrite
        [&](uint16_t address, uint8_t value) {
            if (m_traceEnabled && g_currTraceInfo) {
                g_currTraceInfo->AddMemoryAccess(address, value, false);
            }

            if (auto bp = m_breakpoints.Get(address)) {
                if (bp->enabled && (bp->type == Breakpoint::Type::Write ||
                                    bp->type == Breakpoint::Type::ReadWrite)) {
                    BreakIntoDebugger();
                    Printf("Watchpoint hit at $%04x (write value $%02x)\n", address, value);
                }
            }
        });

    // Load up commands for debugger to execute on startup
    std::ifstream fin("startup.txt");
    if (fin) {
        std::string command;
        while (std::getline(fin, command)) {
            if (!command.empty())
                m_pendingCommands.push(command);
        }
    }
}

void Debugger::Reset() {
    m_cpuCyclesLeft = 0;
    // We want to keep our breakpoints when resetting a game
    // m_breakpoints.Reset();
    m_cpuCyclesTotal = 0;
    m_cpuCyclesLeft = 0;
    g_instructionTraceBuffer.Clear();
    g_currTraceInfo = nullptr;
}

void Debugger::BreakIntoDebugger() {
    m_breakIntoDebugger = true;
    SetFocusConsole();
}

void Debugger::ResumeFromDebugger() {
    m_breakIntoDebugger = false;
    SetFocusMainWindow();
}

void Debugger::SyncInstructionHash(SyncProtocol& syncProtocol,
                                   int numInstructionsExecutedThisFrame) {
    if (syncProtocol.IsStandalone())
        return;

    bool hashMismatch = false;

    // Sync hashes and compare
    if (syncProtocol.IsServer()) {
        syncProtocol.SendValue(ConnectionType::Server, m_instructionHash);

    } else if (syncProtocol.IsClient()) {
        uint32_t serverInstructionHash{};
        syncProtocol.RecvValue(ConnectionType::Client, serverInstructionHash);
        hashMismatch = m_instructionHash != serverInstructionHash;
    }

    // Sync whether to continue or stop
    if (syncProtocol.IsClient()) {
        syncProtocol.SendValue(ConnectionType::Client, hashMismatch);
    } else if (syncProtocol.IsServer()) {
        syncProtocol.RecvValue(ConnectionType::Server, hashMismatch);
    }

    if (hashMismatch) {
        Errorf("Instruction hash mismatch in last %d instructions\n",
               numInstructionsExecutedThisFrame);

        // @TODO: Unfortunately, we still deadlock when multiple instances call BreakIntoDebugger at
        // the same time, so for now, just don't do it.
        // BreakIntoDebugger();
        m_breakIntoDebugger = true;

        if (syncProtocol.IsServer())
            syncProtocol.ShutdownServer();
        else
            syncProtocol.ShutdownClient();
    }
}

bool Debugger::FrameUpdate(double frameTime, const Input& input, const EmuEvents& emuEvents,
                           RenderContext& renderContext, AudioContext& audioContext,
                           SyncProtocol& syncProtocol) {

    int numInstructionsExecutedThisFrame = 0;

    auto PrintOp = [&](const InstructionTraceInfo& traceInfo) {
        if (m_traceEnabled) {
            ::PrintOp(traceInfo, m_symbolTable);
        }
    };

    auto PrintLastOp = [&] {
        if (m_traceEnabled) {
            InstructionTraceInfo traceInfo;
            if (g_instructionTraceBuffer.PeekBack(traceInfo)) {
                PrintOp(traceInfo);
            }
        }
    };

    auto ExecuteInstruction = [&] {
        try {
            InstructionTraceInfo traceInfo;
            if (m_traceEnabled) {
                g_currTraceInfo = &traceInfo;
                PreOpWriteTraceInfo(traceInfo, m_cpu->Registers(), *m_memoryBus);
            }

            // HACK: init to non-zero so that if an exception is thrown when executing instruction,
            // we end up collecting the last instruction in our trace - see "if (cpuCycles == 0)"
            // check in onExit lambda below. @TODO: fix this!
            cycles_t cpuCycles = 99999;

            // In case exception is thrown below, we still want to add the current instruction trace
            // info, so wrap the call in a ScopedExit
            auto onExit = MakeScopedExit([&] {
                if (m_traceEnabled) {

                    // If the CPU didn't do anything (e.g. waiting for interrupts), we have nothing
                    // to log or hash
                    if (cpuCycles == 0) {
                        g_currTraceInfo = nullptr;
                        return;
                    }

                    PostOpWriteTraceInfo(traceInfo, m_cpu->Registers(), cpuCycles);
                    g_instructionTraceBuffer.PushBackMoveFront(traceInfo);
                    g_currTraceInfo = nullptr;

                    // Compute running hash of instruction trace
                    if (!syncProtocol.IsStandalone())
                        m_instructionHash = HashInstructionTraceInfo(m_instructionHash, traceInfo);

                    ++numInstructionsExecutedThisFrame;
                }
            });

            cpuCycles = m_cpu->ExecuteInstruction(m_via->IrqEnabled(), m_via->FirqEnabled());

            if (cpuCycles > 0)
                ++m_instructionCount;

            cycles_t effectiveCycles = cpuCycles == 0 ? 10 : cpuCycles;

            m_via->Update(effectiveCycles, input, renderContext, audioContext);
            return effectiveCycles;

        } catch (std::exception& ex) {
            Printf("Exception caught:\n%s\n", ex.what());
            PrintLastOp();
        } catch (...) {
            Printf("Unknown exception caught\n");
            PrintLastOp();
        }
        BreakIntoDebugger();
        return static_cast<cycles_t>(0);
    };

    for (auto& event : emuEvents) {
        if (std::holds_alternative<EmuEvent::BreakIntoDebugger>(event.type)) {
            BreakIntoDebugger();
            break;
        }
    }

    // Set default console colors
    Platform::ScopedConsoleColor defaultColor(Platform::ConsoleColor::White,
                                              Platform::ConsoleColor::Black);

    if (m_breakIntoDebugger || !m_pendingCommands.empty()) {

        Platform::ScopedConsoleColor defaultOutputColor(Platform::ConsoleColor::LightAqua);

        std::string inputCommand;

        if (!m_pendingCommands.empty()) {
            inputCommand = m_pendingCommands.front();
            m_pendingCommands.pop();
            Printf("%s\n", inputCommand.c_str());
            FlushStream(ConsoleStream::Output);

        } else {
            auto prompt =
                FormattedString<>("$%04x (%s)>", m_cpu->Registers().PC, m_lastCommand.c_str());
            inputCommand = Platform::ConsoleReadLine(prompt);
        }

        auto tokens = Tokenize(inputCommand);

        // If no input, repeat last command
        if (tokens.size() == 0) {
            inputCommand = m_lastCommand;
            tokens = Tokenize(m_lastCommand);
        }

        bool validCommand = true;

        if (tokens.size() == 0) {
            // Don't do anything (no command entered yet)

        } else if (tokens[0] == "quit" || tokens[0] == "q") {
            return false;

        } else if (tokens[0] == "help" || tokens[0] == "h") {
            PrintHelp();

        } else if (tokens[0] == "continue" || tokens[0] == "c") {
            // First 'step' current instruction, otherwise if we have a breakpoint on it we will
            // end up breaking immediately on it again (we won't actually continue)
            ExecuteInstruction();
            ResumeFromDebugger();

        } else if (tokens[0] == "step" || tokens[0] == "s") {
            // "Step into"
            ExecuteInstruction();

            // Handle optional number of steps parameter
            if (tokens.size() > 1) {
                m_numInstructionsToExecute = StringToIntegral<int64_t>(tokens[1]) - 1;
                if (m_numInstructionsToExecute.value() > 0) {
                    ResumeFromDebugger();
                }
            } else {
                PrintLastOp();
            }

        } else if (tokens[0] == "until" || tokens[0] == "u") {
            if (tokens.size() > 1) {
                uint16_t address = StringToIntegral<uint16_t>(tokens[1]);
                auto bp = m_breakpoints.Add(Breakpoint::Type::Instruction, address);
                bp->autoDelete = true;
                ResumeFromDebugger();
            } else {
                validCommand = false;
            }

        } else if (tokens[0] == "break" || tokens[0] == "b") {
            validCommand = false;
            if (tokens.size() > 1) {
                uint16_t address = StringToIntegral<uint16_t>(tokens[1]);
                if (auto bp = m_breakpoints.Add(Breakpoint::Type::Instruction, address)) {
                    Printf("Added breakpoint at $%04x\n", address);
                    validCommand = true;
                }
            }

        } else if (tokens[0] == "watch" || tokens[0] == "rwatch" || tokens[0] == "awatch") {
            validCommand = false;
            if (tokens.size() > 1) {
                uint16_t address = StringToIntegral<uint16_t>(tokens[1]);

                auto type = tokens[0][0] == 'w' ? Breakpoint::Type::Write
                                                : tokens[0][0] == 'r' ? Breakpoint::Type::Read
                                                                      : Breakpoint::Type::ReadWrite;

                if (auto bp = m_breakpoints.Add(type, address)) {
                    Printf("Added watchpoint at $%04x\n", address);
                    validCommand = true;
                }
            }

        } else if (tokens[0] == "delete") {
            validCommand = false;
            if (tokens.size() > 1) {
                int breakpointIndex = std::stoi(tokens[1]);
                if (auto bp = m_breakpoints.RemoveAtIndex(breakpointIndex)) {
                    Printf("Deleted breakpoint %d at $%04x\n", breakpointIndex, bp->address);
                    validCommand = true;
                } else {
                    Printf("Invalid breakpoint specified\n");
                }
            }

        } else if (tokens[0] == "enable") {
            validCommand = false;
            if (tokens.size() > 1) {
                size_t breakpointIndex = std::stoi(tokens[1]);
                if (auto bp = m_breakpoints.GetAtIndex(breakpointIndex)) {
                    bp->enabled = true;
                    Printf("Enabled breakpoint %d at $%04x\n", breakpointIndex, bp->address);
                    validCommand = true;
                } else {
                    Printf("Invalid breakpoint specified\n");
                }
            }

        } else if (tokens[0] == "disable") {
            validCommand = false;
            if (tokens.size() > 1) {
                size_t breakpointIndex = std::stoi(tokens[1]);
                if (auto bp = m_breakpoints.GetAtIndex(breakpointIndex)) {
                    bp->enabled = false;
                    Printf("Disabled breakpoint %d at $%04x\n", breakpointIndex, bp->address);
                    validCommand = true;
                } else {
                    Printf("Invalid breakpoint specified\n");
                }
            }

        } else if (tokens[0] == "info") {
            if (tokens.size() > 1 && (tokens[1] == "registers" || tokens[1] == "reg")) {
                PrintRegisters(m_cpu->Registers());
                Printf("\n");
            } else if (tokens.size() > 1 && (tokens[1] == "break")) {
                Printf("Breakpoints:\n");
                Platform::ScopedConsoleColor scc;
                for (size_t i = 0; i < m_breakpoints.Num(); ++i) {
                    auto bp = m_breakpoints.GetAtIndex(i);
                    Platform::SetConsoleColor(bp->enabled ? Platform::ConsoleColor::LightGreen
                                                          : Platform::ConsoleColor::LightRed);
                    Printf("%3d: $%04x\t%-20s%s\n", i, bp->address,
                           Breakpoint::TypeToString(bp->type),
                           bp->enabled ? "Enabled" : "Disabled");
                }
            } else {
                validCommand = false;
            }

        } else if (tokens[0] == "print" || tokens[0] == "p") {
            if (tokens.size() > 1) {
                uint16_t address = StringToIntegral<uint16_t>(tokens[1]);
                uint8_t value = m_memoryBus->Read(address);
                Printf("$%04x = $%02x (%d)\n", address, value, value);
            } else {
                validCommand = false;
            }

        } else if (tokens[0] == "set") { // e.g. set $addr=value
            validCommand = false;
            if (tokens.size() > 1) {
                // Recombine the tokens after 'set' into a string so we can split it on '='. We
                // have to do this because the user may have put whitespace around '='.
                auto assignment =
                    std::accumulate(tokens.begin() + 1, tokens.end(), std::string(""));
                auto args = Split(assignment, "=");
                if (args.size() == 2) {
                    auto address = StringToIntegral<uint16_t>(args[0]);
                    auto value = StringToIntegral<uint8_t>(args[1]);
                    m_memoryBus->Write(address, value);
                    validCommand = true;
                }
            }

        } else if (tokens[0] == "loadsymbols") {
            if (tokens.size() > 1 && LoadUserSymbolsFile(tokens[1].c_str(), m_symbolTable)) {
                Printf("Loaded symbols from %s\n", tokens[1].c_str());
            } else {
                validCommand = false;
            }

        } else if (tokens[0] == "toggle") {
            if (tokens.size() > 1) {
                if (tokens[1] == "color") {
                    m_colorEnabled = !m_colorEnabled;
                    SetColorEnabled(m_colorEnabled);
                    Printf("Color %s\n", m_colorEnabled ? "enabled" : "disabled");
                } else if (tokens[1] == "trace") {
                    m_traceEnabled = !m_traceEnabled;
                    Printf("Trace %s\n", m_traceEnabled ? "enabled" : "disabled");
                }
            } else {
                validCommand = false;
            }

        } else if (tokens[0] == "option") {
            if (tokens.size() > 2) {
                if (tokens[1] == "errors") {
                    auto& policy = ErrorHandler::g_policy;
                    if (tokens[2] == "ignore")
                        policy = ErrorHandler::Policy::Ignore;
                    else if (tokens[2] == "log")
                        policy = ErrorHandler::Policy::Log;
                    else if (tokens[2] == "fail")
                        policy = ErrorHandler::Policy::Fail;
                    else
                        validCommand = false;
                }
            } else {
                validCommand = false;
            }

        } else if (tokens[0] == "trace" || tokens[0] == "t") {
            size_t numLines = 10;
            const char* outFileName = nullptr;

            try {
                for (size_t i = 1; i < tokens.size(); ++i) {
                    auto& token = tokens[i];
                    if (token == "-n") {
                        numLines = StringToIntegral<size_t>(tokens.at(i + 1));
                        ++i;
                    } else if (token == "-f") {
                        outFileName = tokens.at(i + 1).c_str();
                        ++i;
                    } else {
                        throw std::exception();
                    }
                }
            } catch (...) {
                validCommand = false;
            }

            if (validCommand) {
                FileStream fileStream;
                ScopedOverridePrintStream ScopedOverridePrintStream;

                if (outFileName) {
                    if (!fileStream.Open(outFileName, "w+"))
                        Printf("Failed to create trace file\n");
                    else {
                        Printf("Writing trace to %s\n", outFileName);
                        ScopedOverridePrintStream.SetPrintStream(fileStream.Get());
                    }
                }

                // Allow Ctrl+C to break out of printing ops (can be very long)
                bool bKeepPrinting = true;
                auto scopedConsoleCtrlHandler = ScopedConsoleCtrlHandler([&bKeepPrinting] {
                    bKeepPrinting = false;
                    return true;
                });

                std::vector<InstructionTraceInfo> buffer(numLines);
                auto numInstructions = g_instructionTraceBuffer.PeekBack(buffer.data(), numLines);
                buffer.resize(numInstructions);
                for (auto& traceInfo : buffer) {
                    PrintOp(traceInfo);

                    if (!bKeepPrinting)
                        break;
                }
            }
        } else {
            validCommand = false;
        }

        if (validCommand) {
            m_lastCommand = inputCommand;
        } else {
            Printf("Invalid command: %s\n", inputCommand.c_str());
        }
    } else { // Not broken into debugger (running)

        const double cpuHz = 6'000'000.0 / 4.0; // Frequency of the CPU (cycles/second)
        const double cpuCyclesThisFrame = cpuHz * frameTime;

        // Execute as many instructions that can fit in this time slice (plus one more at most)
        m_cpuCyclesLeft += cpuCyclesThisFrame;
        while (m_cpuCyclesLeft > 0) {
            if (auto bp = m_breakpoints.Get(m_cpu->Registers().PC)) {
                if (bp->type == Breakpoint::Type::Instruction) {
                    if (bp->autoDelete) {
                        m_breakpoints.Remove(m_cpu->Registers().PC);
                        BreakIntoDebugger();
                    } else if (bp->enabled) {
                        Printf("Breakpoint hit at %04x\n", bp->address);
                        BreakIntoDebugger();
                    }
                }
            }

            if (m_breakIntoDebugger) {
                m_cpuCyclesLeft = 0;
                break;
            }

            const cycles_t elapsedCycles = ExecuteInstruction();

            m_cpuCyclesTotal += elapsedCycles;
            m_cpuCyclesLeft -= elapsedCycles;

            if (m_numInstructionsToExecute && (--m_numInstructionsToExecute.value() == 0)) {
                m_numInstructionsToExecute = {};
                BreakIntoDebugger();
            }

            if (m_breakIntoDebugger) {
                m_cpuCyclesLeft = 0;
                break;
            }
        }
    }

    SyncInstructionHash(syncProtocol, numInstructionsExecutedThisFrame);

    return true;
}
