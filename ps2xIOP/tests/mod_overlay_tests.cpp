// Resource replacement: path safety, mod folder indexing, CVM container overlay (in place / smaller / larger / relocated),
// and the integration with the virtual CDVD disc (DISC override, container sectors, reversible when disabled).
#include "iop_compat_test_support.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/core/iop_memory.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_cpu.h"
#include "ps2x/iop/mod_overlay.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

using namespace ps2x::iop;
namespace fs = std::filesystem;

namespace
{
    constexpr size_t kSector = 2048;
    constexpr size_t kHeader = 0x1800;

    void both(uint8_t *p, uint32_t v, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            p[i] = uint8_t(v >> (8 * i));
            p[n + i] = uint8_t(v >> (8 * (n - 1 - i)));
        }
    }

    void record(uint8_t *p, uint32_t lsn, uint32_t size, const std::string &name, bool dir)
    {
        p[0] = uint8_t((33 + name.size() + 1) & ~size_t(1));
        both(p + 2, lsn, 4);
        both(p + 10, size, 4);
        p[25] = dir ? 2 : 0;
        both(p + 28, 1, 2);
        p[32] = uint8_t(name.size());
        std::memcpy(p + 33, name.data(), name.size());
    }

    // CVM = 0x1800 header + ISO: A.TXT (1 sector, 0x11), SUB/B.RAX (3 sectors, 0x22), C.BIN (1 sector, 0x33), 60 sectors total.
    std::vector<uint8_t> makeCvm()
    {
        std::vector<uint8_t> b(kHeader + 60 * kSector, 0);
        std::memcpy(b.data(), "CVMH", 4);
        uint8_t *iso = b.data() + kHeader;
        auto *p = iso + 16 * kSector;
        p[0] = 1;
        std::memcpy(p + 1, "CD001", 5);
        p[6] = 1;
        both(p + 80, 60, 4);
        both(p + 120, 1, 2);
        both(p + 124, 1, 2);
        both(p + 128, kSector, 2);
        record(p + 156, 20, kSector, std::string(1, '\0'), true);
        p = iso + 17 * kSector;
        p[0] = 255;
        std::memcpy(p + 1, "CD001", 5);
        p[6] = 1;
        uint8_t *root = iso + 20 * kSector;
        size_t o = 0;
        record(root + o, 20, kSector, std::string(1, '\0'), true);
        o += root[o];
        record(root + o, 20, kSector, std::string(1, '\1'), true);
        o += root[o];
        record(root + o, 30, kSector, "A.TXT;1", false);
        o += root[o];
        record(root + o, 21, kSector, "SUB", true);
        o += root[o];
        record(root + o, 50, kSector, "C.BIN;1", false);
        uint8_t *sub = iso + 21 * kSector;
        o = 0;
        record(sub + o, 21, kSector, std::string(1, '\0'), true);
        o += sub[o];
        record(sub + o, 20, kSector, std::string(1, '\1'), true);
        o += sub[o];
        record(sub + o, 40, 3 * kSector - 100, "B.RAX;1", false);
        std::fill(iso + 30 * kSector, iso + 31 * kSector, 0x11);
        std::fill(iso + 40 * kSector, iso + 43 * kSector, 0x22);
        std::fill(iso + 50 * kSector, iso + 51 * kSector, 0x33);
        return b;
    }

    struct Parsed
    {
        uint32_t lsn = 0, size = 0;
        bool found = false;
    };

    // Reads a directory record by name out of the sectors the overlay serves (what the game's CVM driver does).
    Parsed lookup(mods::CvmOverlay &cvm, uint32_t dirLsn, const std::string &name)
    {
        std::vector<uint8_t> s(kSector);
        cvm.readSector(uint32_t(kHeader / kSector) + dirLsn, s.data());
        size_t o = 0;
        while (o < kSector && s[o])
        {
            if (std::string(reinterpret_cast<char *>(&s[o + 33]), s[o + 32]) == name)
            {
                auto le = [&](size_t i) { return uint32_t(s[o + i]) | uint32_t(s[o + i + 1]) << 8 | uint32_t(s[o + i + 2]) << 16 | uint32_t(s[o + i + 3]) << 24; };
                auto be = [&](size_t i) { return uint32_t(s[o + i]) << 24 | uint32_t(s[o + i + 1]) << 16 | uint32_t(s[o + i + 2]) << 8 | uint32_t(s[o + i + 3]); };
                if (le(2) != be(6) || le(10) != be(14))
                    return {};
                return {le(2), le(10), true};
            }
            o += s[o];
        }
        return {};
    }

    void writeFile(const fs::path &path, size_t bytes, uint8_t value)
    {
        fs::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary);
        std::string data(bytes, char(value));
        f.write(data.data(), std::streamsize(data.size()));
    }

    struct Host : iop_test::Host
    {
        std::string root, modRoot;
        std::map<uint64_t, std::string> files;
        uint64_t serial = 10;
        std::string hostPath(HostPathKind k) const override
        {
            return k == HostPathKind::CdRoot ? root : k == HostPathKind::ModRoot ? modRoot : std::string{};
        }
        uint64_t openHostFile(std::string_view path) override
        {
            if (!fs::is_regular_file(path))
                return 0;
            files[++serial] = std::string(path);
            return serial;
        }
        bool hostFileSize(uint64_t h, uint64_t &size) const override
        {
            auto it = files.find(h);
            if (it == files.end())
                return false;
            size = fs::file_size(it->second);
            return true;
        }
        bool readHostFile(uint64_t h, uint64_t offset, void *dst, size_t size, size_t &got) override
        {
            got = 0;
            auto it = files.find(h);
            if (it == files.end())
                return false;
            std::ifstream f(it->second, std::ios::binary);
            f.seekg(std::streamoff(offset));
            f.read(static_cast<char *>(dst), std::streamsize(size));
            got = size_t(f.gcount());
            return true;
        }
        void closeHostFile(uint64_t h) override { files.erase(h); }
    };
} // namespace

int main()
{
    const auto temp = fs::temp_directory_path() / ("mod-overlay-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        size_t checks = 0;
        auto check = [&](bool ok, const char *why) { iop_test::require(ok, why); ++checks; };

        // --- path safety -------------------------------------------------------------------------------------------------
        check(mods::normalizeLogicalPath("Sub\\b.rax;1") == "SUB/B.RAX", "separators/case/version");
        check(mods::normalizeLogicalPath("a/b/") == "A/B", "trailing separator");
        for (const char *bad : {"", "/a", "\\a", "a//b", "..", "a/../b", "../a", "./a", "C:/x", "a:b", "a/b?", "a/b*", "a|b", "a\x01", "a ", "a.", "a/b.."})
            check(mods::normalizeLogicalPath(bad).empty(), bad);

        // --- mod folder index --------------------------------------------------------------------------------------------
        fs::create_directories(temp);
        const fs::path mods = temp / "mods";
        writeFile(mods / "ST" / "a.txt", 100, 0xAA);                      // smaller than the original sector
        writeFile(mods / "ST" / "sub" / "B.RAX", 3 * kSector - 100, 0xBB); // same size as the original
        writeFile(mods / "DISC" / "SYSTEM.CNF", 7, 0xCC);
        writeFile(mods / "ST" / "missing.bin", 10, 0xDD);                 // not in the container
        mods::ModRoot index;
        std::vector<std::string> log;
        check(index.open(mods, [&](const std::string &t) { log.push_back(t); }), "mod folder opens");
        check(index.fileCount() == 4 && index.files("ST") && index.files("ST")->count("A.TXT") && index.files("ST")->count("SUB/B.RAX"), "index keys");
        check(index.files("DISC") && index.files("DISC")->count("SYSTEM.CNF") && !index.files("VOICE"), "containers");
        check(!mods::ModRoot().open(temp / "nope", nullptr), "missing folder is not an error, just no mods");
        {
            mods::ModRoot none;
            check(none.empty() && none.fileCount() == 0, "empty set");
        }

        // --- CVM overlay -------------------------------------------------------------------------------------------------
        const std::vector<uint8_t> cvmBytes = makeCvm();
        auto reader = [&](uint64_t off, void *dst, size_t n) {
            if (off + n > cvmBytes.size())
                return false;
            std::memcpy(dst, cvmBytes.data() + off, n);
            return true;
        };
        auto cvm = mods::CvmOverlay::build("ST", cvmBytes.size(), reader, *index.files("ST"), nullptr);
        check(cvm != nullptr, "overlay built");
        check(cvm->headerBytes() == kHeader && cvm->appendedSectors() == 0 && cvm->size() == cvmBytes.size(), "in-place edits do not grow the container");
        check(cvm->replacements().size() == 2, "the file that is not in the container is ignored");
        std::vector<uint8_t> s(kSector);
        const auto a = lookup(*cvm, 20, "A.TXT;1");
        check(a.found && a.lsn == 30 && a.size == 100, "A.TXT keeps its extent, size patched to the smaller file");
        check(cvm->readSector(uint32_t(kHeader / kSector) + 30, s.data()) && s[0] == 0xAA && s[99] == 0xAA && s[100] == 0, "A.TXT sector holds the mod bytes, zero padded");
        const auto b = lookup(*cvm, 21, "B.RAX;1");
        check(b.found && b.lsn == 40 && b.size == 3 * kSector - 100, "B.RAX same size");
        check(cvm->readSector(uint32_t(kHeader / kSector) + 42, s.data()) && s[0] == 0xBB && s[kSector - 101] == 0xBB && s[kSector - 100] == 0, "last sector of B.RAX");
        const auto c = lookup(*cvm, 20, "C.BIN;1");
        check(c.found && c.lsn == 50 && c.size == kSector, "unreplaced file record untouched");
        check(cvm->readSector(uint32_t(kHeader / kSector) + 50, s.data()) && s[0] == 0x33 && s[kSector - 1] == 0x33, "unreplaced data untouched");
        check(cvm->readSector(0, s.data()) && std::memcmp(s.data(), "CVMH", 4) == 0, "container header passes through");

        // larger than the original extent -> relocated after the original data, volume size patched
        fs::remove(mods / "ST" / "a.txt");
        writeFile(mods / "ST" / "C.BIN", 5 * kSector + 17, 0xEE);
        mods::ModRoot index2;
        index2.open(mods, nullptr);
        auto big = mods::CvmOverlay::build("ST", cvmBytes.size(), reader, *index2.files("ST"), nullptr);
        check(big != nullptr && big->appendedSectors() == 6 && big->size() == kHeader + 66 * kSector, "grown container size");
        const auto big_c = lookup(*big, 20, "C.BIN;1");
        check(big_c.found && big_c.lsn == 60 && big_c.size == 5 * kSector + 17, "relocated record");
        check(big->readSector(uint32_t(kHeader / kSector) + 60, s.data()) && s[0] == 0xEE, "relocated first sector");
        check(big->readSector(uint32_t(kHeader / kSector) + 65, s.data()) && s[16] == 0xEE && s[17] == 0, "relocated tail zero padded");
        check(big->readSector(uint32_t(kHeader / kSector) + 16, s.data()) && s[80] == 66 && s[87] == 66, "volume space size patched (both-endian)");
        check(big->readSector(uint32_t(kHeader / kSector) + 50, s.data()) && s[0] == 0x33, "the old extent is still readable");
        check(mods::CvmOverlay::build("ST", cvmBytes.size(), reader, {}, nullptr) == nullptr, "no mod files -> no overlay");
        {
            std::vector<uint8_t> notCvm(kHeader + 20 * kSector, 0);
            auto r = [&](uint64_t off, void *dst, size_t n) { if (off + n > notCvm.size()) return false; std::memcpy(dst, notCvm.data() + off, n); return true; };
            check(mods::CvmOverlay::build("ST", notCvm.size(), r, *index2.files("ST"), nullptr) == nullptr, "not an ISO: untouched");
        }

        // --- integration with the virtual disc --------------------------------------------------------------------------
        const fs::path disc = temp / "disc";
        fs::create_directories(disc);
        {
            std::ofstream f(disc / "ST.CVM", std::ios::binary);
            f.write(reinterpret_cast<const char *>(cvmBytes.data()), std::streamsize(cvmBytes.size()));
            writeFile(disc / "SYSTEM.CNF", 50, 0x5A);
        }
        Host host;
        host.root = disc.string();
        detail::IopMemory memory;
        detail::IopKernel kernel(memory);
        detail::IopCdvd cd(host, memory, kernel);
        memory.zeroRam(0x4000, 4 * kSector);
        auto call = [&](uint16_t ordinal, uint32_t x, uint32_t y, uint32_t z = 0) {
            detail::IopCpuState cpu{};
            cpu.gpr[4] = x;
            cpu.gpr[5] = y;
            cpu.gpr[6] = z;
            check(cd.dispatchImport(ordinal, cpu), "import unhandled");
            return cpu.gpr[2];
        };
        auto lsnOf = [&](const std::string &path) {
            memory.writeRam(0x1000, path.c_str(), path.size() + 1);
            if (call(10, 0x1100, 0x1000) != 1)
                return uint32_t(0xFFFFFFFFu);
            uint32_t n = 0;
            memory.readRam(0x1100, &n, 4);
            return n;
        };
        auto readFirstByte = [&](uint32_t lsn) {
            if (call(6, lsn, 1, 0x4000) != 1)
                return -1;
            uint8_t v = 0;
            memory.readRam(0x4000, &v, 1);
            return int(v);
        };
        auto firstFileByte = [&](const char *name) {
            const uint32_t lsn = lsnOf(name);
            return lsn == 0xFFFFFFFFu ? -2 : readFirstByte(lsn);
        };

        host.modRoot.clear();
        cd.reset();
        check(firstFileByte("cdrom0:/SYSTEM.CNF;1") == 0x5A, "no mod folder: original disc file");
        const uint32_t plainCvm = lsnOf("cdrom0:/ST.CVM;1");
        check(plainCvm != 0xFFFFFFFFu, "container found");
        check(readFirstByte(plainCvm + 3 + 30) == 0x11, "no mod folder: original container data");

        host.modRoot = mods.string();
        cd.reset();
        check(firstFileByte("cdrom0:/SYSTEM.CNF;1") == 0xCC, "DISC/SYSTEM.CNF replaces the root file");
        {
            const uint32_t l = lsnOf("cdrom0:/SYSTEM.CNF;1");
            call(6, l, 1, 0x4000);
            uint8_t v[8] = {};
            memory.readRam(0x4000, v, 8);
            check(v[6] == 0xCC && v[7] == 0, "replacement bytes then zero padding");
        }
        const uint32_t modCvm = lsnOf("cdrom0:/ST.CVM;1");
        check(readFirstByte(modCvm + 3 + 60) == 0xEE, "container relocated extent is served through the virtual disc");
        check(readFirstByte(modCvm + 3 + 42) == 0xBB, "container in-place extent is served through the virtual disc");
        check(readFirstByte(modCvm + 3 + 50) == 0x33, "unreplaced container data untouched");

        host.modRoot = "off-not-a-folder";
        cd.reset();
        check(firstFileByte("cdrom0:/SYSTEM.CNF;1") == 0x5A && readFirstByte(lsnOf("cdrom0:/ST.CVM;1") + 3 + 42) == 0x22,
              "pointing at a missing folder (or disabling) restores the original behaviour");

        fs::remove_all(temp);
        std::cout << "PASS " << checks << " resource replacement checks\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::error_code ec;
        fs::remove_all(temp, ec);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
