#ifndef PS2X_ISO_IMAGE_H
#define PS2X_ISO_IMAGE_H

// Read-only ISO9660 disc image reader (header-only). Used by the launcher to recognise a user's disc image and to copy
// its files into the per-user cache once; the emulation itself keeps working on a plain folder, so the mod / translation
// overlay machinery is untouched.
//
// Supported: 2048-byte sectors (.iso) and raw 2352-byte sectors (Mode 1 / Mode 2 Form 1: .bin/.img/.mdf), contiguous files
// and multi-extent files, ISO level 1-3 names (";1" version suffix removed). Not supported: Joliet-only discs, UDF-only discs,
// interleaved files.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::iso
{
    struct Entry
    {
        std::string name;                      // without ";1"
        bool directory = false;
        bool multiExtentOpen = false;          // more extents of the same file follow (directory record flag)
        uint64_t size = 0;                     // bytes (all extents)
        std::vector<std::pair<uint32_t, uint64_t>> extents; // (first LSN, byte length)
    };

    class Image
    {
    public:
        // Empty string on success, otherwise a message for the user.
        std::string open(const std::filesystem::path &path)
        {
            file_.close();
            file_.clear();
            file_.open(path, std::ios::binary);
            if (!file_)
                return "Cannot open " + path.string();
            path_ = path;
            std::error_code ec;
            fileBytes_ = std::filesystem::file_size(path, ec);
            if (ec || fileBytes_ < 18u * 2048u)
                return path.filename().string() + " is too small to be a disc image.";

            struct Layout { uint32_t sector; uint32_t dataOffset; };
            static const Layout layouts[] = {{2048, 0}, {2352, 16}, {2352, 24}};
            bool found = false;
            for (const Layout &layout : layouts)
            {
                std::vector<uint8_t> pvd;
                sectorSize_ = layout.sector;
                dataOffset_ = layout.dataOffset;
                if (readSectors(16, 1, pvd) && pvd[0] == 1 && std::string(pvd.begin() + 1, pvd.begin() + 6) == "CD001")
                {
                    found = true;
                    root_ = parseRecord(pvd.data() + 156, 34);
                    break;
                }
            }
            if (!found)
                return path.filename().string() + " does not look like an ISO9660 disc image (no primary volume descriptor).";
            if (!root_ || !root_->directory)
                return "The disc image has no readable root directory.";
            return {};
        }

        const std::filesystem::path &path() const { return path_; }

        std::vector<Entry> list(const Entry &directory) const
        {
            std::vector<Entry> entries;
            if (!directory.directory || directory.extents.empty())
                return entries;
            std::vector<uint8_t> data;
            if (!readSectors(directory.extents[0].first, static_cast<uint32_t>((directory.size + 2047u) / 2048u), data))
                return entries;
            size_t pos = 0;
            while (pos < static_cast<size_t>(directory.size) && pos < data.size())
            {
                const uint8_t length = data[pos];
                if (length == 0)
                {
                    pos = (pos / 2048u + 1u) * 2048u; // records never cross a sector
                    continue;
                }
                if (pos + length > data.size())
                    break;
                std::optional<Entry> entry = parseRecord(data.data() + pos, length);
                pos += length;
                if (!entry || entry->name.empty() || entry->name == "." || entry->name == "..")
                    continue;
                if (!entries.empty() && !entry->directory && !entries.back().directory && entries.back().name == entry->name && entries.back().multiExtentOpen)
                {
                    entries.back().extents.insert(entries.back().extents.end(), entry->extents.begin(), entry->extents.end());
                    entries.back().size += entry->size;
                    entries.back().multiExtentOpen = entry->multiExtentOpen;
                    continue;
                }
                entries.push_back(*entry);
            }
            return entries;
        }

        const Entry &root() const { return *root_; }

        // "SYSTEM.CNF", "OVERLAY/OVL00.BIN" (case-insensitive).
        std::optional<Entry> find(const std::string &path) const
        {
            Entry current = *root_;
            size_t begin = 0;
            while (begin <= path.size())
            {
                size_t end = path.find_first_of("/\\", begin);
                if (end == std::string::npos)
                    end = path.size();
                const std::string part = path.substr(begin, end - begin);
                begin = end + 1;
                if (part.empty())
                    continue;
                bool matched = false;
                for (const Entry &child : list(current))
                {
                    if (equalsNoCase(child.name, part))
                    {
                        current = child;
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                    return std::nullopt;
            }
            return current;
        }

        // Whole file into memory (small files only: SYSTEM.CNF, the boot ELF).
        bool readFile(const Entry &entry, std::vector<uint8_t> &out, uint64_t maxBytes = 64ull << 20) const
        {
            out.clear();
            if (entry.directory || entry.size > maxBytes)
                return false;
            out.reserve(static_cast<size_t>(entry.size));
            return stream(entry, [&](const uint8_t *data, size_t size) { out.insert(out.end(), data, data + size); return true; });
        }

        // Streams a file in chunks; the sink returns false to stop.
        bool stream(const Entry &entry, const std::function<bool(const uint8_t *, size_t)> &sink) const
        {
            std::vector<uint8_t> chunk;
            for (const auto &[lsn, bytes] : entry.extents)
            {
                uint64_t remaining = bytes;
                uint32_t sector = lsn;
                while (remaining > 0)
                {
                    const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>((remaining + 2047u) / 2048u, 512u));
                    if (!readSectors(sector, count, chunk))
                        return false;
                    const size_t useful = static_cast<size_t>(std::min<uint64_t>(remaining, static_cast<uint64_t>(count) * 2048u));
                    if (!sink(chunk.data(), useful))
                        return false;
                    remaining -= useful;
                    sector += count;
                }
            }
            return true;
        }

        // Sum of all file sizes below `directory`, and the file count.
        void measure(const Entry &directory, uint64_t &bytes, uint64_t &files) const
        {
            for (const Entry &child : list(directory))
            {
                if (child.directory)
                    measure(child, bytes, files);
                else
                {
                    bytes += child.size;
                    ++files;
                }
            }
        }

        using Progress = std::function<void(uint64_t bytesDone, uint64_t bytesTotal, const std::string &currentFile)>;

        // Copies every file below `directory` into `destination` (created). Empty string on success.
        std::string extract(const Entry &directory, const std::filesystem::path &destination, uint64_t total, uint64_t &done, const Progress &progress) const
        {
            std::error_code ec;
            std::filesystem::create_directories(destination, ec);
            if (ec)
                return "Cannot create " + destination.string();
            for (const Entry &child : list(directory))
            {
                if (!safeName(child.name))
                    return "The disc image has an unusable file name: " + child.name;
                const std::filesystem::path target = destination / child.name;
                if (child.directory)
                {
                    if (std::string error = extract(child, target, total, done, progress); !error.empty())
                        return error;
                    continue;
                }
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                if (!out)
                    return "Cannot write " + target.string();
                const bool ok = stream(child, [&](const uint8_t *data, size_t size) {
                    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
                    done += size;
                    if (progress)
                        progress(done, total, child.name);
                    return static_cast<bool>(out);
                });
                out.close();
                if (!ok || !out)
                    return "Failed while copying " + child.name + " (disk full or damaged image?)";
            }
            return {};
        }

        static bool safeName(const std::string &name)
        {
            if (name.empty() || name == "." || name == "..")
                return false;
            return name.find_first_of("<>:\"/\\|?*") == std::string::npos && name.back() != ' ' && name.back() != '.';
        }

    private:
        static bool equalsNoCase(const std::string &a, const std::string &b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        static uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

        bool readSectors(uint32_t lsn, uint32_t count, std::vector<uint8_t> &out) const
        {
            out.assign(static_cast<size_t>(count) * 2048u, 0);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint64_t offset = static_cast<uint64_t>(lsn + i) * sectorSize_ + dataOffset_;
                if (offset + 2048u > fileBytes_)
                    return false;
                file_.clear();
                file_.seekg(static_cast<std::streamoff>(offset));
                file_.read(reinterpret_cast<char *>(out.data()) + static_cast<size_t>(i) * 2048u, 2048);
                if (file_.gcount() != 2048)
                    return false;
            }
            return true;
        }

        std::optional<Entry> parseRecord(const uint8_t *p, size_t available) const
        {
            if (available < 34 || p[0] < 34 || p[0] > available)
                return std::nullopt;
            const uint8_t nameLength = p[32];
            if (33u + nameLength > p[0])
                return std::nullopt;
            Entry entry;
            entry.directory = (p[25] & 2u) != 0;
            entry.multiExtentOpen = (p[25] & 0x80u) != 0;
            const uint32_t lsn = le32(p + 2);
            entry.size = le32(p + 10);
            entry.extents.emplace_back(lsn, entry.size);
            if (nameLength == 1 && p[33] == 0)
                entry.name = ".";
            else if (nameLength == 1 && p[33] == 1)
                entry.name = "..";
            else
            {
                entry.name.assign(reinterpret_cast<const char *>(p + 33), nameLength);
                if (const size_t semicolon = entry.name.rfind(';'); semicolon != std::string::npos)
                    entry.name.erase(semicolon);
                if (!entry.name.empty() && entry.name.back() == '.')
                    entry.name.pop_back();
            }
            return entry;
        }

        mutable std::ifstream file_;
        std::filesystem::path path_;
        uint64_t fileBytes_ = 0;
        uint32_t sectorSize_ = 2048;
        uint32_t dataOffset_ = 0;
        std::optional<Entry> root_;
    };
}

#endif
