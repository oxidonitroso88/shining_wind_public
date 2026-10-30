#pragma once

// Resource replacement ("mods"): lets the virtual disc serve modified files from an external folder without touching the
// original game files and without rebuilding an ISO. Design and measurements: docs/resource-replacement-2026-10-13.md
//
// Layout of the mod folder (case-insensitive, `/` or `\` separators, an optional ";1" version suffix is ignored):
//   <mods>/DISC/<path>        a file of the disc root directory tree, e.g. DISC/OVERLAY/OVL09.BIN, DISC/MODULES/...
//   <mods>/<CONTAINER>/<path> a file inside a CRI CVM container of the disc root: ST/ACTION.RAX replaces ACTION.RAX in ST.CVM,
//                             VOICE/... replaces files in VOICE.CVM
// A replaced file may have any size: inside a container it is written in place when it fits the original extent and is
// appended after the original data otherwise; the container's ISO9660 records are patched in the sectors the game reads.
// Nothing is written to disk: everything happens when sectors are served.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::mods
{
    using LogFn = std::function<void(const std::string &)>;

    // Normalised logical path: upper case ASCII, `/` separators, no ";1" suffix. Returns an empty string for anything unsafe
    // (empty, absolute, drive letters, `..`, control characters or characters illegal in Windows names).
    std::string normalizeLogicalPath(std::string_view path);

    // Folder with replacement resources: PS2X_MODS=<folder> (PS2X_MODS=off or 0 disables), otherwise a folder called `mods` in
    // the working directory if it exists. Empty = no mods = the original behaviour. Shared by the IOP and the EE side.
    std::string resolveModFolder();

    // The mod folder and the files found in it, indexed by container. Symbolic links and anything that resolves outside the
    // folder are ignored. A folder that does not exist (or is not configured) is simply an empty set of mods.
    class ModRoot
    {
    public:
        // Returns false (and leaves the set empty) when `root` is not an existing directory.
        bool open(const std::filesystem::path &root, const LogFn &log);

        bool empty() const { return containers_.empty(); }
        const std::filesystem::path &root() const { return root_; }
        // Files of one container ("DISC", "ST", ...), keyed by normalised logical path.
        const std::map<std::string, std::filesystem::path> *files(const std::string &container) const;
        size_t fileCount() const;

    private:
        std::filesystem::path root_;
        std::map<std::string, std::map<std::string, std::filesystem::path>> containers_;
    };

    // A CRI CVM container (a small header followed by a plain ISO9660 image) as the game sees it with some files replaced.
    class CvmOverlay
    {
    public:
        using Reader = std::function<bool(uint64_t offset, void *destination, size_t size)>;

        // Parses the ISO9660 tree of the original container and plans the replacements. Returns nullptr when the container is
        // not a recognisable CVM or none of the mod files matches a file of it.
        static std::shared_ptr<CvmOverlay> build(const std::string &container,
                                                 uint64_t originalSize,
                                                 Reader original,
                                                 const std::map<std::string, std::filesystem::path> &modFiles,
                                                 const LogFn &log);

        // Size of the container including data appended for replaced files that outgrew their original extent.
        uint64_t size() const { return size_; }
        // Reads one 2048 byte sector of the container as the game must see it (zero padded past the end).
        bool readSector(uint32_t sector, uint8_t *destination);

        struct Replacement
        {
            std::string path;
            uint32_t originalLsn = 0, originalSize = 0, lsn = 0, size = 0;
            bool relocated = false;
            std::filesystem::path hostFile;
        };
        const std::vector<Replacement> &replacements() const { return replacements_; }
        uint32_t headerBytes() const { return headerBytes_; }
        uint32_t appendedSectors() const { return appendedSectors_; }

    private:
        struct Extent
        {
            uint32_t begin = 0, count = 0;
            size_t replacement = 0;
        };

        bool readModBytes(size_t replacement, uint64_t offset, uint8_t *destination, size_t size);

        std::string container_;
        Reader original_;
        uint64_t originalSize_ = 0, size_ = 0;
        uint32_t headerBytes_ = 0, isoSectors_ = 0, appendedSectors_ = 0;
        std::vector<Replacement> replacements_;
        std::vector<Extent> extents_; // sorted by begin (ISO sector numbers)
        std::map<uint32_t, std::vector<uint8_t>> patchedSectors_; // directory sectors and the volume descriptor
        LogFn log_;
        std::vector<bool> served_;
        std::set<uint32_t> patchedServed_;
        std::set<size_t> staleServed_;
    };
} // namespace ps2x::iop::mods
