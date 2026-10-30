#include "ps2recomp/Emitters/function_table_emitter.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/ps2_recompiler.h"
#include "ps2recomp/recompiler_reporter.h"
#include "ps2recomp/types.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ps2recomp
{
    FunctionTableEmitter::FunctionTableEmitter(CodeGenerator &codeGenerator)
        : m_codeGenerator(codeGenerator)
    {
    }

    std::string FunctionTableEmitter::emit(const std::vector<Function> &functions, const std::map<uint32_t, std::string> &stubs)
    {
        (void)stubs;

        CodeGenerator &cg = m_codeGenerator;
        std::vector<std::pair<uint32_t, std::string>> entries;
        std::unordered_set<uint32_t> registeredAddresses;

        auto addEntry = [&](uint32_t address, const std::string &name)
        {
            if (name.empty())
            {
                return;
            }
            if ((address & 3u) != 0u)
            {
                std::ostringstream oss;
                oss << "Unaligned function table entry for " << name << " at 0x" << std::hex << address;

                if (cg.m_reporter)
                {
                    cg.m_reporter->errorAt("function-table", name, address, oss.str());
                }

                throw std::runtime_error(oss.str());
            }
            if (!registeredAddresses.insert(address).second)
            {
                return;
            }
            entries.emplace_back(address, name);
        };

        std::vector<std::pair<uint32_t, std::string>> normalFunctions;
        std::vector<std::pair<uint32_t, std::string>> stubFunctions;
        std::vector<std::pair<uint32_t, std::string>> systemCallFunctions;
        std::vector<std::pair<uint32_t, std::string>> libraryFunctions;

        for (const auto &function : functions)
        {
            if (!function.isRecompiled && !function.isStub && !function.isSkipped)
                continue;

            std::string generatedName = cg.getFunctionName(function.start);

            if (function.isSkipped)
            {
                libraryFunctions.emplace_back(function.start, generatedName);
            }
            else if (function.isStub)
            {
                const auto target = PS2Recompiler::resolveStubTarget(function.name);
                if (target == StubTarget::Syscall)
                {
                    systemCallFunctions.emplace_back(function.start, generatedName);
                }
                else
                {
                    stubFunctions.emplace_back(function.start, generatedName);
                }
            }
            else
            {
                normalFunctions.emplace_back(function.start, generatedName);
            }
        }

        if (cg.m_bootstrapInfo.valid && !cg.isSupplement())
        {
            std::string entryTarget = cg.m_bootstrapInfo.entryName;
            if (entryTarget.empty())
            {
                entryTarget = cg.getFunctionName(cg.m_bootstrapInfo.entry);
            }
            if (entryTarget.empty())
            {
                throw std::runtime_error("No entry function name available for registration.");
            }
            addEntry(cg.m_bootstrapInfo.entry, entryTarget);
        }

        for (const auto &[address, name] : normalFunctions)
        {
            addEntry(address, name);
        }

        for (const auto &[ownerStart, targets] : cg.m_resumeEntryTargetsByOwner)
        {
            const std::string ownerName = cg.getFunctionName(ownerStart);
            if (ownerName.empty())
            {
                continue;
            }

            for (uint32_t target : targets)
            {
                addEntry(target, ownerName);
            }
        }

        for (const auto &[address, name] : stubFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : systemCallFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : libraryFunctions)
        {
            addEntry(address, name);
        }

        std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b)
                  { return a.first < b.first; });

        if (cg.isOverlay())
        {
            return emitOverlay(entries);
        }
        if (cg.isSupplement())
        {
            return emitSupplement(entries);
        }

        uint32_t tableBase = 0u;
        uint32_t tableEnd = 0u;
        uint32_t slotCount = 0u;
        if (!entries.empty())
        {
            tableBase = entries.front().first & ~3u;
            tableEnd = (entries.back().first + 4u + 3u) & ~3u;
            slotCount = (tableEnd - tableBase) >> 2;
        }

        std::stringstream ss;
        ss << "#include \"ps2_runtime.h\"\n";
        ss << "#include <" << cg.functionHeaderName() << ">\n";
        ss << "#include \"ps2_stubs.h\"\n";
        ss << "#include <" << cg.stubsHeaderName() << ">\n";
        ss << "#include \"ps2_syscalls.h\"\n\n";

        ss << "extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x" << std::hex << tableBase << "u;\n";
        ss << "extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x" << std::hex << tableEnd << "u;\n";
        ss << "extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = " << std::dec << slotCount << "u;\n";
        ss << "PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[" << std::dec << (slotCount == 0u ? 1u : slotCount) << "u] = {};\n\n";

        ss << "namespace {\n";
        ss << "struct GeneratedFunctionTableInitializer {\n";
        ss << "    GeneratedFunctionTableInitializer() {\n";
        for (const auto &[address, name] : entries)
        {
            const uint32_t slot = (address - tableBase) >> 2;
            ss << "        g_ps2RecompiledFunctionTable[" << std::dec << slot << "] = " << name
               << "; // 0x" << std::hex << address << std::dec << "\n";
        }
        ss << "    }\n";
        ss << "};\n";
        ss << "static const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;\n";
        ss << "}\n";

        return ss.str();
    }

    namespace
    {
        // FNV-1a 64 over the bytes; must match ps2_overlay::hashBytes (ps2xRuntime/include/ps2_overlay.h).
        uint64_t overlayHashBytes(const uint8_t *data, size_t size)
        {
            uint64_t hash = 0xcbf29ce484222325ull;
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= data[i];
                hash *= 0x100000001b3ull;
            }
            return hash;
        }
    }

    // A code overlay shares its load address with other overlays, so its functions cannot go in the dense main table.
    // The registration is a descriptor (name, executable range, content hash, sample words, sorted entries) that
    // the runtime selects by looking at the guest memory (ps2xRuntime/include/ps2_overlay.h).
    std::string FunctionTableEmitter::emitOverlay(const std::vector<std::pair<uint32_t, std::string>> &entries)
    {
        CodeGenerator &cg = m_codeGenerator;

        // The first executable section is the code that identifies the overlay; an optional second one holds code that the linker
        // left after the data (static constructors): its entries are registered too, but it is not part of the identity.
        const Section *code = nullptr;
        const Section *extra = nullptr;
        for (const auto &section : cg.m_sections)
        {
            if (section.isCode && section.size > 0 && section.data != nullptr)
            {
                if (code == nullptr)
                {
                    code = &section;
                }
                else if (extra == nullptr)
                {
                    extra = &section;
                }
                else
                {
                    throw std::runtime_error("overlay '" + cg.m_overlayName + "' has more than two executable sections");
                }
            }
        }
        const uint32_t extraBegin = extra ? extra->address : 0u;
        const uint32_t extraEnd = extra ? extra->address + extra->size : 0u;
        if (code == nullptr || (code->address & 3u) != 0u || (code->size & 3u) != 0u)
        {
            throw std::runtime_error("overlay '" + cg.m_overlayName + "' has no valid (word aligned) executable section");
        }

        const uint32_t codeBegin = code->address;
        const uint32_t codeEnd = code->address + code->size;
        const uint32_t words = code->size >> 2;
        constexpr uint32_t kMaxSamples = 32u;
        const uint32_t sampleCount = std::min(words, kMaxSamples);
        const uint32_t strideWords = std::max(1u, words / sampleCount);

        std::stringstream ss;
        ss << "#include \"ps2_runtime.h\"\n";
        ss << "#include \"ps2_overlay.h\"\n";
        ss << "#include <" << cg.functionHeaderName() << ">\n\n";
        ss << "// Overlay '" << cg.m_overlayName << "': code 0x" << std::hex << codeBegin << "-0x" << codeEnd << std::dec
           << ", " << entries.size() << " entries (generated, do not edit)\n";
        ss << "namespace {\n";
        ss << "const ps2_overlay::Function k_" << cg.m_overlayName << "_functions[] = {\n";
        for (const auto &[address, name] : entries)
        {
            if ((address < codeBegin || address >= codeEnd) && (address < extraBegin || address >= extraEnd))
            {
                throw std::runtime_error("overlay '" + cg.m_overlayName + "': entry " + name + " outside the code range");
            }
            ss << "    {0x" << std::hex << address << "u, " << name << "},\n";
        }
        ss << "};\n";
        ss << "const uint32_t k_" << cg.m_overlayName << "_samples[] = {\n    ";
        for (uint32_t i = 0; i < sampleCount; ++i)
        {
            uint32_t word = 0u;
            std::memcpy(&word, code->data + static_cast<size_t>(i) * strideWords * 4u, 4u);
            ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << word << "u, " << std::setfill(' ');
            if ((i & 7u) == 7u && i + 1u < sampleCount)
            {
                ss << "\n    ";
            }
        }
        ss << "\n};\n";
        ss << "const ps2_overlay::Descriptor k_" << cg.m_overlayName << "_descriptor = {\n";
        ss << "    \"" << cg.m_overlayName << "\",\n";
        ss << "    0x" << std::hex << codeBegin << "u, 0x" << codeEnd << "u,\n";
        ss << "    0x" << std::setw(16) << std::setfill('0') << overlayHashBytes(code->data, code->size) << "ull,\n"
           << std::setfill(' ');
        ss << "    k_" << cg.m_overlayName << "_samples, " << std::dec << sampleCount << "u, " << (strideWords * 4u) << "u,\n";
        ss << "    k_" << cg.m_overlayName << "_functions, " << entries.size() << "u,\n";
        ss << "    0x" << std::hex << extraBegin << "u, 0x" << extraEnd << "u,\n" << std::dec;
        ss << "};\n";
        ss << "struct Register_" << cg.m_overlayName << " {\n";
        ss << "    Register_" << cg.m_overlayName << "() { ps2_overlay::registerOverlay(&k_" << cg.m_overlayName << "_descriptor); }\n";
        ss << "};\n";
        ss << "const Register_" << cg.m_overlayName << " g_register_" << cg.m_overlayName << ";\n";
        ss << "}\n";
        return ss.str();
    }

    // Extra functions of the main program (see RecompilerConfig::supplementName): registered into the dense main table
    // at start-up, only in slots that are still empty (ps2_overlay::registerSupplement).
    std::string FunctionTableEmitter::emitSupplement(const std::vector<std::pair<uint32_t, std::string>> &entries)
    {
        CodeGenerator &cg = m_codeGenerator;
        const std::string name = cg.auxiliaryName();
        std::stringstream ss;
        ss << "#include \"ps2_runtime.h\"\n";
        ss << "#include \"ps2_overlay.h\"\n";
        ss << "#include <" << cg.functionHeaderName() << ">\n\n";
        ss << "// Supplement '" << name << "': " << entries.size() << " entries (generated, do not edit)\n";
        ss << "namespace {\n";
        ss << "const ps2_overlay::Function k_" << name << "_functions[] = {\n";
        for (const auto &[address, fn] : entries)
        {
            ss << "    {0x" << std::hex << address << "u, " << fn << "},\n";
        }
        ss << "};\n";
        ss << "struct Register_" << name << " {\n";
        ss << "    Register_" << name << "() { ps2_overlay::registerSupplement(\"" << name << "\", k_" << name << "_functions, "
           << std::dec << entries.size() << "u); }\n";
        ss << "};\n";
        ss << "const Register_" << name << " g_register_" << name << ";\n";
        ss << "}\n";
        return ss.str();
    }
}
