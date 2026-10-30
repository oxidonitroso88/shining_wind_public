// Code overlays (ps2_overlay.h): selection of the overlay that is really installed in guest memory, substitution
// (loading another overlay invalidates the functions of the previous one), and the integration with PS2Runtime lookups.
#include "MiniTest.h"
#include "ps2_overlay.h"
#include "ps2_runtime.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/types.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kBase = 0x4000u; // code range [kBase, kBase + kCodeBytes) in a small fake RDRAM
    constexpr uint32_t kCodeBytes = 0x200u;
    constexpr uint32_t kRamBytes = 0x10000u;

    int g_calledA1 = 0, g_calledA2 = 0, g_calledB1 = 0, g_calledB2 = 0, g_calledC1 = 0;
    void fnA1(uint8_t *, R5900Context *, PS2Runtime *) { ++g_calledA1; }
    void fnA2(uint8_t *, R5900Context *, PS2Runtime *) { ++g_calledA2; }
    void fnB1(uint8_t *, R5900Context *, PS2Runtime *) { ++g_calledB1; }
    void fnB2(uint8_t *, R5900Context *, PS2Runtime *) { ++g_calledB2; }
    void fnC1(uint8_t *, R5900Context *, PS2Runtime *) { ++g_calledC1; }

    // A synthetic overlay: deterministic code words, with its own sample words and hash computed the way the generator does.
    struct FakeOverlay
    {
        std::string name;
        std::vector<uint8_t> code;
        std::vector<uint32_t> samples;
        std::vector<ps2_overlay::Function> functions;
        ps2_overlay::Descriptor descriptor{};

        FakeOverlay(const char *n, uint32_t seed, std::vector<ps2_overlay::Function> fns)
            : name(n), code(kCodeBytes), functions(std::move(fns))
        {
            for (uint32_t i = 0; i < kCodeBytes / 4; ++i)
            {
                const uint32_t word = 0x24000000u ^ (seed * 2654435761u) ^ (i * 40503u); // never all-equal across seeds
                std::memcpy(code.data() + i * 4, &word, 4);
            }
            finalize();
        }

        // Recompute samples/hash after the code bytes were edited (used to build overlays that share their samples).
        void finalize()
        {
            const uint32_t words = kCodeBytes / 4;
            const uint32_t sampleCount = 32;
            const uint32_t stride = words / sampleCount * 4;
            samples.clear();
            for (uint32_t i = 0; i < sampleCount; ++i)
            {
                uint32_t w;
                std::memcpy(&w, code.data() + i * stride, 4);
                samples.push_back(w);
            }
            descriptor = {name.c_str(), kBase, kBase + kCodeBytes, ps2_overlay::hashBytes(code.data(), code.size()),
                          samples.data(), sampleCount, stride, functions.data(), static_cast<uint32_t>(functions.size())};
        }

        void install(std::vector<uint8_t> &ram) const { std::memcpy(ram.data() + kBase, code.data(), code.size()); }
    };

    struct OverlaySetup
    {
        FakeOverlay a{"ovlA", 1, {{kBase + 0x00, fnA1}, {kBase + 0x40, fnA2}, {kBase + 0x44, fnA2}}}; // 0x44 = resume entry in A2
        FakeOverlay b{"ovlB", 2, {{kBase + 0x00, fnB1}, {kBase + 0x80, fnB2}}};                         // same address 0x00 as A
        std::vector<uint8_t> ram = std::vector<uint8_t>(kRamBytes, 0);

        OverlaySetup()
        {
            ps2_overlay::resetForTests();
            ps2_overlay::registerOverlay(&a.descriptor);
            ps2_overlay::registerOverlay(&b.descriptor);
        }
        ~OverlaySetup() { ps2_overlay::resetForTests(); }
    };
}

void register_ps2_overlay_tests()
{
    MiniTest::Case("PS2Overlay", [](TestCase &tc)
    {
        tc.Run("FNV-1a 64 matches the reference vectors (the generator emits the same hash)", [](TestCase &t)
        {
            t.Equals(ps2_overlay::hashBytes(nullptr, 0), 0xcbf29ce484222325ull, "empty input");
            const uint8_t a = 'a';
            t.Equals(ps2_overlay::hashBytes(&a, 1), 0xaf63dc4c8601ec8cull, "'a'");
            const uint8_t foobar[] = {'f', 'o', 'o', 'b', 'a', 'r'};
            t.Equals(ps2_overlay::hashBytes(foobar, 6), 0x85944171f73967e8ull, "'foobar'");
        });

        tc.Run("code after the data (second executable range) resolves while the overlay is installed, and is not part of its identity", [](TestCase &t)
        {
            ps2_overlay::resetForTests();
            constexpr uint32_t kExtra = kBase + kCodeBytes + 0x100u; // static constructors the linker left after the data
            FakeOverlay a("ovlX", 7, {{kBase + 0x00, fnA1}, {kExtra, fnC1}});
            a.descriptor.extraBegin = kExtra;
            a.descriptor.extraEnd = kExtra + 0x40u;
            ps2_overlay::registerOverlay(&a.descriptor);
            std::vector<uint8_t> ram(kRamBytes, 0);
            a.install(ram);
            t.IsTrue(ps2_overlay::inOverlayRegion(kExtra), "the extra range belongs to the overlay region");
            t.IsFalse(ps2_overlay::inOverlayRegion(kExtra + 0x40u), "end of the extra range");
            t.IsTrue(ps2_overlay::resolve(ram.data(), kExtra) == fnC1, "entry in the extra range");
            ram[kExtra + 4u] = 0x5A; // data that changes at run time next to the code does not break the identification
            t.IsTrue(ps2_overlay::resolve(ram.data(), kBase) == fnA1, "still identified");
            t.IsTrue(ps2_overlay::resolve(ram.data(), kExtra + 0x80u) == nullptr, "outside both ranges");
            ps2_overlay::resetForTests();
        });

        tc.Run("nothing resolves while no registered overlay is installed in memory", [](TestCase &t)
        {
            OverlaySetup s;
            t.IsTrue(ps2_overlay::inOverlayRegion(kBase), "region start");
            t.IsTrue(ps2_overlay::inOverlayRegion(kBase + kCodeBytes - 4), "region last word");
            t.IsFalse(ps2_overlay::inOverlayRegion(kBase - 4), "below the region");
            t.IsFalse(ps2_overlay::inOverlayRegion(kBase + kCodeBytes), "above the region");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == nullptr, "zeroed memory is not an overlay");
            t.IsTrue(ps2_overlay::active() == nullptr, "no active overlay");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), 0x100u) == nullptr, "an address outside the region is never an overlay function");
        });

        tc.Run("the installed overlay is identified by its code, and shared addresses resolve to it only", [](TestCase &t)
        {
            OverlaySetup s;
            s.a.install(s.ram);
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == fnA1, "0x00 -> A1 while A is installed");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase + 0x40) == fnA2, "A2");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase + 0x44) == fnA2, "resume entry inside A2 resolves to A2");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase + 0x80) == nullptr, "B2 does not exist while A is installed");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase + 0x04) == nullptr, "mid-function address that is not an entry");
            t.IsTrue(ps2_overlay::active() == &s.a.descriptor, "A is active");
        });

        tc.Run("installing another overlay invalidates the functions of the previous one", [](TestCase &t)
        {
            OverlaySetup s;
            s.a.install(s.ram);
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase + 0x40) == fnA2, "A2 resolves with A installed");
            const uint64_t switchesBefore = ps2_overlay::switchCount();

            s.b.install(s.ram); // the guest copies another overlay over the first, then flushes the I-cache
            ps2_overlay::notifyCodeFlush();
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == fnB1, "the shared address now resolves to B1, never to A1");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase + 0x80) == fnB2, "B2 resolves");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase + 0x40) == nullptr, "A2 is gone with A");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase + 0x44) == nullptr, "A2's resume entry is gone too");
            t.IsTrue(ps2_overlay::active() == &s.b.descriptor, "B is active");
            t.IsTrue(ps2_overlay::switchCount() > switchesBefore, "the switch was recorded");

            s.a.install(s.ram); // and back, this time without an explicit flush: the sampled words still detect the change
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == fnA1, "back to A1");
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase + 0x80) == nullptr, "B2 is gone");
        });

        tc.Run("a flush re-verifies the full code: overlays that share their sampled words are still told apart", [](TestCase &t)
        {
            OverlaySetup s;
            FakeOverlay c{"ovlC", 1, {{kBase + 0x00, fnC1}}}; // same words as A ...
            // ... except one word that is not among the 32 samples (stride = 16 bytes; word 1 is not sampled)
            const uint32_t other = 0xDEADBEEFu;
            std::memcpy(c.code.data() + 4, &other, 4);
            c.finalize();
            t.IsTrue(c.descriptor.sampleWords[0] == s.a.descriptor.sampleWords[0] &&
                         std::memcmp(c.samples.data(), s.a.samples.data(), s.a.samples.size() * 4) == 0,
                     "precondition: identical samples");
            t.IsTrue(c.descriptor.codeHash != s.a.descriptor.codeHash, "precondition: different hash");
            ps2_overlay::registerOverlay(&c.descriptor);

            s.a.install(s.ram);
            ps2_overlay::notifyCodeFlush();
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == fnA1, "A installed");
            c.install(s.ram);
            ps2_overlay::notifyCodeFlush();
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == fnC1, "after the flush the full hash selects C, not A");
        });

        tc.Run("corrupted or partially copied code matches no overlay; restoring it re-activates the overlay", [](TestCase &t)
        {
            OverlaySetup s;
            s.a.install(s.ram);
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == fnA1, "A installed");

            std::memcpy(s.ram.data() + kBase + 0x1F0, "\xFF\xFF\xFF\xFF", 4); // last word, not sampled
            ps2_overlay::notifyCodeFlush();
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == nullptr, "a modified code range is not A any more");
            t.IsTrue(ps2_overlay::active() == nullptr, "no overlay is active");

            s.a.install(s.ram);
            ps2_overlay::notifyCodeFlush();
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == fnA1, "restored");
        });

        tc.Run("data after the code range may change freely (overlay variables) without deactivating the overlay", [](TestCase &t)
        {
            OverlaySetup s;
            s.a.install(s.ram);
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == fnA1, "A installed");
            std::memset(s.ram.data() + kBase + kCodeBytes, 0x5A, 0x100);
            ps2_overlay::notifyCodeFlush();
            t.IsTrue(ps2_overlay::resolve(s.ram.data(), kBase) == fnA1, "still A");
        });

        tc.Run("PS2Runtime lookups use the installed overlay (lookupFunction and hasFunction)", [](TestCase &t)
        {
            ps2_overlay::resetForTests();
            FakeOverlay a{"ovlA", 1, {{kBase + 0x00, fnA1}, {kBase + 0x40, fnA2}}};
            FakeOverlay b{"ovlB", 2, {{kBase + 0x00, fnB1}, {kBase + 0x80, fnB2}}};
            ps2_overlay::registerOverlay(&a.descriptor);
            ps2_overlay::registerOverlay(&b.descriptor);

            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");
            uint8_t *rdram = runtime.memory().getRDRAM();

            t.IsFalse(runtime.hasFunction(kBase), "nothing installed yet");
            std::memcpy(rdram + kBase, a.code.data(), a.code.size());
            ps2_overlay::notifyCodeFlush();
            t.IsTrue(runtime.hasFunction(kBase), "A installed: entry exists");
            t.IsTrue(runtime.lookupFunction(kBase) == fnA1, "lookupFunction -> A1");
            t.IsFalse(runtime.hasFunction(kBase + 0x80), "B2 is not an entry of A");

            std::memcpy(rdram + kBase, b.code.data(), b.code.size());
            ps2_overlay::notifyCodeFlush();
            t.IsTrue(runtime.lookupFunction(kBase) == fnB1, "lookupFunction -> B1 after B replaced A");
            t.IsTrue(runtime.hasFunction(kBase + 0x80), "B2 is an entry of B");
            t.IsFalse(runtime.hasFunction(kBase + 0x40), "A2 vanished");
            ps2_overlay::resetForTests();
        });

        tc.Run("recompiler: an overlay registers a descriptor (names, header, hash, sorted entries) instead of the dense table", [](TestCase &t)
        {
            using namespace ps2recomp;
            // code section 0x4000..0x4020 (8 words); function at 0x4000 and a resume alias at 0x4008
            static const uint32_t words[8] = {0x03E00008u, 0u, 0x27BDFFF0u, 0x03E00008u, 0x27BD0010u, 0u, 0u, 0u};
            std::vector<Section> sections;
            Section code{};
            code.name = ".text";
            code.address = 0x4000u;
            code.size = sizeof(words);
            code.isCode = true;
            code.data = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(words));
            sections.push_back(code);

            std::vector<Symbol> symbols;
            CodeGenerator cg(symbols, sections);
            cg.setOverlayName("ovlT");
            t.Equals(cg.functionHeaderName(), std::string("ps2_ovlT_functions.h"), "header name");
            t.IsTrue(cg.isOverlay(), "overlay mode");

            std::unordered_map<uint32_t, std::string> renames{{0x4000u, "ovlT_FUN_00004000_0x4000"}, {0x4008u, "ovlT_FUN_00004008_0x4008"}};
            cg.setRenamedFunctions(renames);
            std::vector<Function> functions;
            for (uint32_t start : {0x4000u, 0x4008u})
            {
                Function f{};
                f.name = "ovlT_FUN";
                f.start = start;
                f.end = start + 8;
                f.isRecompiled = true;
                functions.push_back(f);
            }
            const std::string text = cg.generateFunctionRegistration(functions, {});
            t.IsTrue(text.find("#include <ps2_ovlT_functions.h>") != std::string::npos, "includes the overlay header");
            t.IsTrue(text.find("g_ps2RecompiledFunctionTable") == std::string::npos, "never touches the dense main table");
            t.IsTrue(text.find("ps2_overlay::registerOverlay") != std::string::npos, "registers the descriptor");
            const size_t first = text.find("{0x4000u, ovlT_FUN_00004000_0x4000}");
            const size_t second = text.find("{0x4008u, ovlT_FUN_00004008_0x4008}");
            t.IsTrue(first != std::string::npos && second != std::string::npos && first < second, "entries sorted by address");
            char hash[40];
            std::snprintf(hash, sizeof(hash), "0x%016llxull",
                          static_cast<unsigned long long>(ps2_overlay::hashBytes(reinterpret_cast<const uint8_t *>(words), sizeof(words))));
            t.IsTrue(text.find(hash) != std::string::npos, "the emitted hash equals ps2_overlay::hashBytes of the code bytes");
        });
    });
}
