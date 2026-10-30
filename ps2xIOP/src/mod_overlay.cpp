#include "ps2x/iop/mod_overlay.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <system_error>

namespace ps2x::iop::mods
{
    namespace
    {
        constexpr uint32_t kSector = 2048u;
        constexpr uint32_t kPvdSector = 16u;

        uint32_t sectorsFor(uint64_t bytes) { return static_cast<uint32_t>((bytes + kSector - 1u) / kSector); }

        void putBoth32(uint8_t *p, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
            {
                p[i] = static_cast<uint8_t>(v >> (8 * i));
                p[4 + i] = static_cast<uint8_t>(v >> (8 * (3 - i)));
            }
        }

        uint32_t getLe32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }

        bool illegalComponentChar(unsigned char c)
        {
            return c < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
        }

        void say(const LogFn &log, const std::string &text)
        {
            if (log)
                log(text);
        }
    } // namespace

    std::string normalizeLogicalPath(std::string_view path)
    {
        std::string result;
        size_t begin = 0;
        while (begin <= path.size())
        {
            size_t end = begin;
            while (end < path.size() && path[end] != '/' && path[end] != '\\')
                ++end;
            std::string component(path.substr(begin, end - begin));
            begin = end + 1;
            if (end >= path.size() && component.empty() && !result.empty())
                break; // trailing separator
            if (component.empty() || component == "." || component == "..")
                return {}; // empty component (absolute path, "//"), or an attempt to leave the folder
            if (component.back() == ' ' || component.back() == '.')
                return {}; // Windows would silently strip these and alias another file
            for (char &c : component)
            {
                if (illegalComponentChar(static_cast<unsigned char>(c)))
                    return {};
                c = static_cast<unsigned char>(c) < 0x80u ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            }
            if (!result.empty())
                result.push_back('/');
            result += component;
        }
        const size_t version = result.rfind(";1");
        if (version != std::string::npos && version + 2 == result.size())
            result.resize(version);
        return result;
    }

    std::string resolveModFolder()
    {
        const char *env = std::getenv("PS2X_MODS");
        if (env != nullptr && *env != '\0')
        {
            const std::string value(env);
            return (value == "off" || value == "0") ? std::string() : value;
        }
        std::error_code error;
        const std::filesystem::path fallback = std::filesystem::current_path(error) / "mods";
        return (!error && std::filesystem::is_directory(fallback, error)) ? fallback.string() : std::string();
    }

    bool ModRoot::open(const std::filesystem::path &root, const LogFn &log)
    {
        containers_.clear();
        root_.clear();
        std::error_code error;
        if (root.empty() || !std::filesystem::is_directory(root, error) || error)
            return false;
        const std::filesystem::path canonicalRoot = std::filesystem::canonical(root, error);
        if (error)
            return false;
        root_ = canonicalRoot;

        for (const auto &containerEntry : std::filesystem::directory_iterator(canonicalRoot, error))
        {
            std::error_code entryError;
            if (containerEntry.is_symlink(entryError) || !containerEntry.is_directory(entryError))
                continue;
            const std::string container = normalizeLogicalPath(containerEntry.path().filename().string());
            if (container.empty())
                continue;
            auto &files = containers_[container];
            for (std::filesystem::recursive_directory_iterator it(containerEntry.path(),
                                                                 std::filesystem::directory_options::skip_permission_denied,
                                                                 entryError),
                 end;
                 !entryError && it != end; it.increment(entryError))
            {
                std::error_code fileError;
                if (it->is_symlink(fileError))
                {
                    it.disable_recursion_pending();
                    say(log, "[mods] ignoring symbolic link " + it->path().string());
                    continue;
                }
                if (!it->is_regular_file(fileError))
                    continue;
                const std::filesystem::path relative = std::filesystem::relative(it->path(), containerEntry.path(), fileError);
                const std::string logical = fileError ? std::string() : normalizeLogicalPath(relative.generic_string());
                if (logical.empty())
                {
                    say(log, "[mods] ignoring unsafe name " + it->path().string());
                    continue;
                }
                // Defence in depth: whatever the name looks like, the resolved file must live inside the mod folder.
                const std::filesystem::path resolved = std::filesystem::weakly_canonical(it->path(), fileError);
                const std::filesystem::path inside = fileError ? std::filesystem::path() : resolved.lexically_relative(canonicalRoot);
                if (inside.empty() || *inside.begin() == "..")
                {
                    say(log, "[mods] ignoring file outside the mod folder " + it->path().string());
                    continue;
                }
                if (!files.emplace(logical, it->path()).second)
                    say(log, "[mods] duplicate (case-insensitive) mod file ignored: " + it->path().string());
            }
            if (files.empty())
                containers_.erase(container);
        }
        return true;
    }

    const std::map<std::string, std::filesystem::path> *ModRoot::files(const std::string &container) const
    {
        const auto it = containers_.find(container);
        return it == containers_.end() ? nullptr : &it->second;
    }

    size_t ModRoot::fileCount() const
    {
        size_t count = 0;
        for (const auto &entry : containers_)
            count += entry.second.size();
        return count;
    }

    std::shared_ptr<CvmOverlay> CvmOverlay::build(const std::string &container,
                                                  uint64_t originalSize,
                                                  Reader original,
                                                  const std::map<std::string, std::filesystem::path> &modFiles,
                                                  const LogFn &log)
    {
        if (!original || modFiles.empty())
            return nullptr;

        auto overlay = std::shared_ptr<CvmOverlay>(new CvmOverlay());
        overlay->container_ = container;
        overlay->original_ = original;
        overlay->originalSize_ = originalSize;
        overlay->log_ = log;

        // The ISO9660 image starts after a CVM header whose size we do not assume: look for the volume descriptor.
        std::vector<uint8_t> sector(kSector);
        uint32_t header = 0xFFFFFFFFu;
        for (const uint32_t candidate : {0x1800u, 0x0u, 0x800u, 0x1000u, 0x2000u})
        {
            if (original(candidate + uint64_t(kPvdSector) * kSector, sector.data(), kSector) && sector[0] == 1u &&
                std::memcmp(sector.data() + 1, "CD001", 5) == 0)
            {
                header = candidate;
                break;
            }
        }
        if (header == 0xFFFFFFFFu)
        {
            say(log, "[mods] " + container + ": no ISO9660 volume descriptor found, container left untouched");
            return nullptr;
        }
        overlay->headerBytes_ = header;
        const uint32_t volumeSectors = getLe32(sector.data() + 80);
        const uint32_t rootLsn = getLe32(sector.data() + 156 + 2);
        const uint32_t rootSize = getLe32(sector.data() + 156 + 10);
        if (volumeSectors == 0 || rootSize == 0 || rootSize > (64u << 20))
            return nullptr;
        overlay->isoSectors_ = volumeSectors;
        std::vector<uint8_t> pvd = sector;

        struct Entry
        {
            uint32_t lsn, size, directorySector, recordOffset;
        };
        std::map<std::string, Entry> files;
        std::set<uint32_t> visited;
        struct Pending
        {
            uint32_t lsn, size;
            std::string path;
        };
        std::vector<Pending> pending{{rootLsn, rootSize, std::string()}};
        while (!pending.empty())
        {
            const Pending directory = pending.back();
            pending.pop_back();
            if (!visited.insert(directory.lsn).second || visited.size() > 100000)
                continue;
            std::vector<uint8_t> bytes(directory.size);
            if (!original(header + uint64_t(directory.lsn) * kSector, bytes.data(), bytes.size()))
                continue;
            size_t offset = 0;
            while (offset < bytes.size())
            {
                const uint8_t length = bytes[offset];
                if (length == 0)
                {
                    offset = (offset / kSector + 1u) * kSector; // records never straddle sectors
                    continue;
                }
                if (offset + length > bytes.size() || length < 34u)
                    break;
                const uint8_t nameLength = bytes[offset + 32];
                if (33u + nameLength > length)
                    break;
                const bool isDirectory = (bytes[offset + 25] & 2u) != 0;
                std::string name(reinterpret_cast<const char *>(&bytes[offset + 33]), nameLength);
                const uint32_t lsn = getLe32(&bytes[offset + 2]);
                const uint32_t size = getLe32(&bytes[offset + 10]);
                const bool dots = nameLength == 1u && (name[0] == '\0' || name[0] == '\1');
                if (!dots)
                {
                    const std::string logical = normalizeLogicalPath(directory.path.empty() ? name : directory.path + "/" + name);
                    if (!logical.empty())
                    {
                        if (isDirectory)
                            pending.push_back({lsn, size, logical});
                        else
                            files.emplace(logical, Entry{lsn, size, directory.lsn + static_cast<uint32_t>(offset / kSector),
                                                         static_cast<uint32_t>(offset % kSector)});
                    }
                }
                offset += length;
            }
        }

        uint32_t cursor = volumeSectors;
        for (const auto &[path, modFile] : modFiles)
        {
            const auto found = files.find(path);
            if (found == files.end())
            {
                say(log, "[mods] " + container + "/" + path + ": no such file in " + container + ", ignored");
                continue;
            }
            std::error_code error;
            const uint64_t modSize = std::filesystem::file_size(modFile, error);
            if (error || modSize > 0xFFFFFFFFull)
            {
                say(log, "[mods] " + container + "/" + path + ": unreadable mod file, ignored");
                continue;
            }
            const Entry &entry = found->second;
            Replacement replacement;
            replacement.path = path;
            replacement.originalLsn = entry.lsn;
            replacement.originalSize = entry.size;
            replacement.size = static_cast<uint32_t>(modSize);
            replacement.hostFile = modFile;
            const uint32_t needed = sectorsFor(modSize);
            const uint32_t available = std::max<uint32_t>(sectorsFor(entry.size), 1u);
            if (needed <= available)
            {
                replacement.lsn = entry.lsn;
            }
            else
            {
                replacement.lsn = cursor;
                replacement.relocated = true;
                cursor += needed;
            }
            if (needed != 0)
                overlay->extents_.push_back({replacement.lsn, needed, overlay->replacements_.size()});

            // Patch the directory record the game reads (extent and size, both-endian).
            auto patched = overlay->patchedSectors_.find(entry.directorySector);
            if (patched == overlay->patchedSectors_.end())
            {
                std::vector<uint8_t> copy(kSector);
                if (!original(header + uint64_t(entry.directorySector) * kSector, copy.data(), kSector))
                {
                    say(log, "[mods] " + container + "/" + path + ": cannot read its directory sector, ignored");
                    if (needed != 0)
                        overlay->extents_.pop_back();
                    continue;
                }
                patched = overlay->patchedSectors_.emplace(entry.directorySector, std::move(copy)).first;
            }
            putBoth32(patched->second.data() + entry.recordOffset + 2, replacement.lsn);
            putBoth32(patched->second.data() + entry.recordOffset + 10, replacement.size);
            overlay->replacements_.push_back(std::move(replacement));
        }

        if (overlay->replacements_.empty())
            return nullptr;
        overlay->appendedSectors_ = cursor - volumeSectors;
        if (overlay->appendedSectors_ != 0)
        {
            putBoth32(pvd.data() + 80, volumeSectors + overlay->appendedSectors_);
            overlay->patchedSectors_[kPvdSector] = pvd;
        }
        overlay->size_ = std::max<uint64_t>(originalSize, uint64_t(header) + uint64_t(cursor) * kSector);
        std::sort(overlay->extents_.begin(), overlay->extents_.end(),
                  [](const Extent &a, const Extent &b) { return a.begin < b.begin; });
        for (size_t i = 1; i < overlay->extents_.size(); ++i)
        {
            if (overlay->extents_[i - 1].begin + overlay->extents_[i - 1].count > overlay->extents_[i].begin)
            {
                say(log, "[mods] " + container + ": replaced files overlap on disc, all replacements of this container dropped");
                return nullptr;
            }
        }
        overlay->served_.assign(overlay->replacements_.size(), false);
        for (const Replacement &r : overlay->replacements_)
        {
            say(log, "[mods] " + container + "/" + r.path + ": " + std::to_string(r.originalSize) + " -> " + std::to_string(r.size) +
                         " bytes, " + (r.relocated ? "appended at ISO sector " : "in place at ISO sector ") + std::to_string(r.lsn) +
                         " <- " + r.hostFile.string());
        }
        return overlay;
    }

    bool CvmOverlay::readModBytes(size_t replacement, uint64_t offset, uint8_t *destination, size_t size)
    {
        // Opened per read on purpose: the mod file is never kept locked, so it can be edited while the game runs.
        std::ifstream stream(replacements_[replacement].hostFile, std::ios::binary);
        if (!stream)
            return false;
        stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(reinterpret_cast<char *>(destination), static_cast<std::streamsize>(size));
        return static_cast<size_t>(stream.gcount()) == size;
    }

    bool CvmOverlay::readSector(uint32_t sector, uint8_t *destination)
    {
        std::memset(destination, 0, kSector);
        const uint32_t headerSectors = headerBytes_ / kSector;
        if (sector >= headerSectors)
        {
            const uint32_t iso = sector - headerSectors;
            const auto patched = patchedSectors_.find(iso);
            if (patched != patchedSectors_.end())
            {
                if (patchedServed_.insert(iso).second)
                    say(log_, "[mods] " + container_ + ": serving patched ISO sector " + std::to_string(iso));
                std::memcpy(destination, patched->second.data(), kSector);
                return true;
            }
            // Diagnostics: the game still reads the original extent of a relocated file (it did not use the patched record).
            for (size_t i = 0; i < replacements_.size(); ++i)
            {
                const Replacement &r = replacements_[i];
                if (r.relocated && iso >= r.originalLsn && iso < r.originalLsn + sectorsFor(r.originalSize) &&
                    staleServed_.insert(i).second)
                {
                    say(log_, "[mods] " + container_ + "/" + r.path + ": the game reads the ORIGINAL extent (ISO sector " + std::to_string(iso) +
                                  "), not the relocated one");
                }
            }
            // Extent lookup: the last extent that starts at or before this sector.
            auto next = std::upper_bound(extents_.begin(), extents_.end(), iso,
                                         [](uint32_t value, const Extent &e) { return value < e.begin; });
            if (next != extents_.begin())
            {
                const Extent &extent = *(next - 1);
                if (iso < extent.begin + extent.count)
                {
                    const Replacement &r = replacements_[extent.replacement];
                    if (!served_[extent.replacement])
                    {
                        served_[extent.replacement] = true;
                        say(log_, "[mods] serving " + container_ + "/" + r.path + " from " + r.hostFile.string());
                    }
                    const uint64_t offset = uint64_t(iso - extent.begin) * kSector;
                    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(kSector, r.size - offset));
                    return readModBytes(extent.replacement, offset, destination, wanted);
                }
            }
            if (iso >= isoSectors_)
                return true; // appended area without a file: zeros
        }
        const uint64_t offset = uint64_t(sector) * kSector;
        if (offset >= originalSize_)
            return true;
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(kSector, originalSize_ - offset));
        return original_(offset, destination, wanted);
    }
} // namespace ps2x::iop::mods
