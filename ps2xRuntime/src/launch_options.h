#ifndef PS2X_LAUNCH_OPTIONS_H
#define PS2X_LAUNCH_OPTIONS_H

// Command line for end users: ps2EntryRunner --disc <folder | ELF | disc image> [--data-dir <folder>] [--no-verify]
// The positional form (ps2EntryRunner <ELF>) keeps working unchanged and does not touch any user-data default.
// With --disc (or a double click), memory cards, input config and the optional translation / HD texture packs live in a
// per-user data folder instead of the current directory; an explicit PS2X_MC_ROOT / PS2X_INPUT_CONFIG / PS2X_MODS /
// PS2X_STORY_EN / PS2X_HD_PACK always wins.
//
// A disc image (.iso, .bin, .img, .mdf) is recognised by reading its ISO9660 tree directly. The boot ELF is checked
// against the supported edition before anything is copied; the files are then copied once into <data-dir>/disc/<key>
// and the emulation runs on that plain folder exactly as it does for an extracted disc.

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "iso_image.h"
#include "sha256.h"

namespace ps2x::launch
{
    struct Options
    {
        std::filesystem::path bootPath; // ELF to run (from --disc or the positional argument)
        std::filesystem::path isoPath;  // disc image given to --disc; resolveImage() turns it into bootPath
        std::filesystem::path dataDir;  // per-user data folder (empty = leave the runtime defaults)
        bool discGiven = false;
        bool verify = true; // check the boot ELF against the supported edition (--no-verify skips it)
        std::string error;
    };

    // The recompiled code is tied to this exact executable (SLPM-66671, Shining Wind): another edition would fail in odd ways.
    inline constexpr const char *kSupportedBootElfSha256 = "805c38303d6ae79bd5e425cbcf49f3fb8fd291b9d2ea8f577c1e5b2f04095437";
    inline constexpr uintmax_t kSupportedBootElfSize = 3270888;

    inline std::string lowerAscii(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    inline bool isImagePath(const std::filesystem::path &path)
    {
        const std::string ext = lowerAscii(path.extension().string());
        return ext == ".iso" || ext == ".bin" || ext == ".img" || ext == ".mdf";
    }

    // BOOT2 = cdrom0:\SLPM_666.71;1  ->  SLPM_666.71
    inline std::string bootFileFromText(const std::string &text)
    {
        size_t lineStart = 0;
        while (lineStart <= text.size())
        {
            size_t lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string::npos)
                lineEnd = text.size();
            const std::string line = text.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;
            const size_t key = lowerAscii(line).find("boot2");
            if (key == std::string::npos)
                continue;
            const size_t colon = line.find(':', key);
            if (colon == std::string::npos)
                continue;
            size_t begin = colon + 1;
            while (begin < line.size() && (line[begin] == '\\' || line[begin] == '/' || line[begin] == ' '))
                ++begin;
            size_t end = line.find_first_of(";\r\n ", begin);
            if (end == std::string::npos)
                end = line.size();
            return line.substr(begin, end - begin);
        }
        return {};
    }

    inline std::string bootFileFromSystemCnf(const std::filesystem::path &systemCnf)
    {
        std::ifstream in(systemCnf, std::ios::binary);
        return bootFileFromText(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    }

    // Retail PS2 boot names: SLPM_666.71, SCES_123.45, ...
    inline bool looksLikeBootName(const std::string &name)
    {
        return name.size() >= 8 && name[4] == '_' && std::isalpha(static_cast<unsigned char>(name[0])) &&
               std::isalpha(static_cast<unsigned char>(name[1])) && std::isalpha(static_cast<unsigned char>(name[2])) &&
               std::isalpha(static_cast<unsigned char>(name[3])) && std::isdigit(static_cast<unsigned char>(name[5]));
    }

    // A folder holding the extracted disc -> its boot ELF (SYSTEM.CNF first, then the usual SLxx_nnn.nn names).
    inline std::optional<std::filesystem::path> findBootElf(const std::filesystem::path &discFolder)
    {
        std::error_code ec;
        if (const std::string boot = bootFileFromSystemCnf(discFolder / "SYSTEM.CNF"); !boot.empty())
        {
            const std::filesystem::path candidate = discFolder / boot;
            if (std::filesystem::is_regular_file(candidate, ec))
                return candidate;
        }
        for (const auto &entry : std::filesystem::directory_iterator(discFolder, ec))
        {
            if (entry.is_regular_file(ec) && looksLikeBootName(entry.path().filename().string()))
                return entry.path();
        }
        return std::nullopt;
    }

    inline std::filesystem::path defaultDataDir()
    {
#if defined(_WIN32)
        if (const char *appData = std::getenv("APPDATA"); appData && *appData)
            return std::filesystem::path(appData) / "ShiningWind";
#else
        if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
            return std::filesystem::path(xdg) / "ShiningWind";
        if (const char *home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".local" / "share" / "ShiningWind";
#endif
        return std::filesystem::path("ShiningWind-data");
    }

    inline Options parse(const std::vector<std::string> &args)
    {
        Options options;
        std::string discArg;
        std::string dataArg;
        std::string positional;
        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string &arg = args[i];
            if (arg == "--no-verify")
            {
                options.verify = false;
            }
            else if (arg == "--disc" || arg == "--data-dir")
            {
                if (i + 1 >= args.size() || args[i + 1].empty())
                {
                    options.error = arg + " needs a path";
                    return options;
                }
                (arg == "--disc" ? discArg : dataArg) = args[++i];
            }
            else if (positional.empty() && !arg.empty() && arg.rfind("--", 0) != 0)
            {
                positional = arg;
            }
        }

        if (!discArg.empty())
        {
            options.discGiven = true;
            std::error_code ec;
            const std::filesystem::path disc(discArg);
            if (std::filesystem::is_directory(disc, ec))
            {
                if (auto elf = findBootElf(disc))
                    options.bootPath = *elf;
                else
                    options.error = "No boot ELF (SYSTEM.CNF / SLxx_nnn.nn) found in " + disc.string();
            }
            else if (std::filesystem::is_regular_file(disc, ec))
            {
                if (isImagePath(disc))
                    options.isoPath = disc;
                else
                    options.bootPath = disc;
            }
            else
            {
                options.error = "--disc path does not exist: " + disc.string();
            }
        }
        else if (!positional.empty())
        {
            options.bootPath = positional;
        }

        if (!dataArg.empty())
            options.dataDir = dataArg;
        else if (options.discGiven)
            options.dataDir = defaultDataDir();
        return options;
    }

    // Empty when the boot ELF is the supported edition, otherwise a message for the user.
    inline std::string checkBootElfIdentity(const std::string &fileName, uint64_t size, const std::string &sha256Hex)
    {
        if (size != kSupportedBootElfSize)
            return "This is not the supported edition of Shining Wind (SLPM-66671): " + fileName + " has " + std::to_string(size) +
                   " bytes, expected " + std::to_string(kSupportedBootElfSize) + ".";
        if (sha256Hex != kSupportedBootElfSha256)
            return "This is not the supported edition of Shining Wind (SLPM-66671): the executable differs from the one this port was built from.";
        return {};
    }

    inline std::string verifyBootElf(const std::filesystem::path &elf)
    {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(elf, ec);
        if (ec)
            return "Cannot read " + elf.string();
        return checkBootElfIdentity(elf.filename().string(), size, size == kSupportedBootElfSize ? hash::sha256File(elf) : std::string());
    }

    // ---- disc images -------------------------------------------------------------------------------------------------

    using ImageProgress = std::function<void(uint64_t bytesDone, uint64_t bytesTotal)>;

    inline std::string imageCacheKey(const std::filesystem::path &image)
    {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(image, ec);
        const auto stamp = std::filesystem::last_write_time(image, ec).time_since_epoch().count();
        return std::to_string(size) + "-" + std::to_string(static_cast<long long>(stamp));
    }

    // Recognises the disc image, checks its boot ELF (before copying anything), copies the files once into
    // <dataDir>/disc/<key> and returns that folder's boot ELF. On failure `error` is filled and the result is empty.
    inline std::filesystem::path prepareImage(const std::filesystem::path &imagePath, const std::filesystem::path &dataDir, bool verify,
                                              std::string &error, const ImageProgress &progress = {})
    {
        iso::Image image;
        if (error = image.open(imagePath); !error.empty())
            return {};

        std::string bootName;
        std::vector<uint8_t> bytes;
        if (const auto cnf = image.find("SYSTEM.CNF"); cnf && image.readFile(*cnf, bytes, 1u << 20))
            bootName = bootFileFromText(std::string(bytes.begin(), bytes.end()));
        if (bootName.empty())
        {
            for (const iso::Entry &entry : image.list(image.root()))
                if (!entry.directory && looksLikeBootName(entry.name))
                    bootName = entry.name;
        }
        const auto elfEntry = bootName.empty() ? std::nullopt : image.find(bootName);
        if (!elfEntry || !image.readFile(*elfEntry, bytes))
        {
            error = imagePath.filename().string() + " has no boot executable (SYSTEM.CNF / SLxx_nnn.nn).";
            return {};
        }
        if (verify)
        {
            hash::Sha256 sha;
            sha.update(bytes.data(), bytes.size());
            if (error = checkBootElfIdentity(bootName, bytes.size(), sha.finishHex()); !error.empty())
                return {};
        }

        const std::filesystem::path cacheRoot = dataDir / "disc";
        const std::filesystem::path finalDir = cacheRoot / imageCacheKey(imagePath);
        std::error_code ec;
        if (std::filesystem::exists(finalDir / ".complete", ec) && std::filesystem::is_regular_file(finalDir / bootName, ec))
            return finalDir / bootName; // already copied for this exact image (same size and date)

        uint64_t totalBytes = 0, totalFiles = 0;
        image.measure(image.root(), totalBytes, totalFiles);
        std::filesystem::create_directories(cacheRoot, ec);
        const std::filesystem::space_info space = std::filesystem::space(cacheRoot, ec);
        const uint64_t margin = 256ull << 20;
        if (!ec && space.available < totalBytes + margin)
        {
            error = "Not enough free disk space to prepare the game files: " + std::to_string((totalBytes + margin) >> 20) + " MB needed in " +
                    cacheRoot.string() + ", " + std::to_string(space.available >> 20) + " MB free.";
            return {};
        }

        const std::filesystem::path tempDir = cacheRoot / (".tmp-" + imageCacheKey(imagePath));
        std::filesystem::remove_all(tempDir, ec); // only ever our own leftover from an interrupted copy
        uint64_t done = 0;
        error = image.extract(image.root(), tempDir, totalBytes, done, [&](uint64_t d, uint64_t t, const std::string &) {
            if (progress)
                progress(d, t);
        });
        if (!error.empty())
        {
            std::filesystem::remove_all(tempDir, ec);
            return {};
        }
        {
            std::ofstream marker(tempDir / ".complete", std::ios::binary | std::ios::trunc);
            marker << "image=" << imagePath.filename().string() << "\nfiles=" << totalFiles << "\nbytes=" << totalBytes << "\n";
        }
        std::filesystem::remove_all(finalDir, ec);
        std::filesystem::rename(tempDir, finalDir, ec);
        if (ec)
        {
            error = "Could not finish preparing the game files: " + ec.message();
            return {};
        }
        error.clear();
        return finalDir / bootName;
    }

    inline void printImageProgress(uint64_t done, uint64_t total)
    {
        static int lastPercent = -1;
        const int percent = total ? static_cast<int>(done * 100 / total) : 100;
        if (percent != lastPercent && percent % 5 == 0)
        {
            lastPercent = percent;
            std::cout << "Preparing game files from the disc image: " << percent << "% (one time only)" << std::endl;
        }
    }

    // Resolves --disc <image> once the data folder is known (main calls this after parse / the folder dialog).
    inline void resolveImage(Options &options)
    {
        if (options.isoPath.empty() || !options.bootPath.empty() || !options.error.empty())
            return;
        std::string error;
        const std::filesystem::path elf = prepareImage(options.isoPath, options.dataDir, options.verify, error, printImageProgress);
        if (elf.empty())
            options.error = error;
        else
            options.bootPath = elf;
    }

    // ---- remembered disc / double-click flow -------------------------------------------------------------------------

    inline std::filesystem::path rememberedDiscFile(const std::filesystem::path &dataDir) { return dataDir / "disc_path.txt"; }

    inline std::filesystem::path readRememberedDisc(const std::filesystem::path &dataDir)
    {
        std::ifstream in(rememberedDiscFile(dataDir), std::ios::binary);
        std::string line;
        if (!in || !std::getline(in, line))
            return {};
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
#if defined(__cpp_char8_t)
        return std::filesystem::path(std::u8string(line.begin(), line.end()));
#else
        return std::filesystem::u8path(line);
#endif
    }

    inline void rememberDisc(const std::filesystem::path &dataDir, const std::filesystem::path &disc)
    {
        std::error_code ec;
        std::filesystem::create_directories(dataDir, ec);
        std::ofstream out(rememberedDiscFile(dataDir), std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        const auto utf8 = disc.u8string();
        out.write(reinterpret_cast<const char *>(utf8.data()), static_cast<std::streamsize>(utf8.size()));
        out.put('\n');
    }

    // A chosen folder, ELF or disc image -> verified boot ELF (empty + `why` when it cannot be used).
    inline std::filesystem::path resolveChoice(const std::filesystem::path &chosen, const Options &options, std::string &why,
                                               const ImageProgress &progress)
    {
        std::error_code ec;
        why.clear();
        if (std::filesystem::is_regular_file(chosen, ec) && isImagePath(chosen))
            return prepareImage(chosen, options.dataDir, options.verify, why, progress);
        std::filesystem::path elf;
        if (std::filesystem::is_regular_file(chosen, ec))
            elf = chosen;
        else if (std::filesystem::is_directory(chosen, ec))
            if (auto found = findBootElf(chosen))
                elf = *found;
        if (elf.empty())
        {
            why = "That location has no boot file (SYSTEM.CNF / SLPM_666.71). Pick your game disc image (.iso) or the folder where you extracted the disc.";
            return {};
        }
        if (options.verify)
            why = verifyBootElf(elf);
        return why.empty() ? elf : std::filesystem::path();
    }

    // Double-click flow (no --disc, no ELF): use the remembered disc if it still holds the supported edition, otherwise
    // ask the user with `pick` (a file / folder dialog) and remember the answer. A user who cancels gets an error.
    using DiscPicker = std::function<std::optional<std::filesystem::path>(const std::string &reason)>;

    inline void chooseDiscInteractively(Options &options, const DiscPicker &pick, const ImageProgress &progress = printImageProgress)
    {
        options.dataDir = options.dataDir.empty() ? defaultDataDir() : options.dataDir;
        std::string reason;
        if (const std::filesystem::path saved = readRememberedDisc(options.dataDir); !saved.empty())
        {
            std::error_code ec;
            if (!std::filesystem::exists(saved, ec))
            {
                reason = "The saved game location no longer exists: " + saved.string();
            }
            else if (const std::filesystem::path elf = resolveChoice(saved, options, reason, progress); !elf.empty())
            {
                options.bootPath = elf;
                options.discGiven = true;
                return;
            }
        }
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const std::optional<std::filesystem::path> chosen = pick ? pick(reason) : std::nullopt;
            if (!chosen)
            {
                options.error = "No game selected.";
                return;
            }
            const std::filesystem::path elf = resolveChoice(*chosen, options, reason, progress);
            if (elf.empty())
                continue;
            options.bootPath = elf;
            options.discGiven = true;
            rememberDisc(options.dataDir, *chosen);
            return;
        }
        options.error = reason.empty() ? "No valid game selected." : reason;
    }

    inline void setEnvIfUnset(const char *name, const std::string &value)
    {
        if (const char *current = std::getenv(name); current && *current)
            return;
#if defined(_WIN32)
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 0);
#endif
    }

    // Point the runtime's existing environment hooks at the per-user data folder (never overrides an explicit setting).
    // Optional add-ons are plain drop-in folders, so nothing here depends on absolute paths:
    //   <data>/mods/               replacement resources (translated .RAX / .DPK produced by the translation installer)
    //   <data>/mods/story_en.tsv   English story lines
    //   <data>/hd_packs/           HD texture packs (a pack folder or a folder of packs)
    inline void applyDataDir(const Options &options)
    {
        if (options.dataDir.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories(options.dataDir, ec);
        const std::string stem = options.bootPath.stem().string();
        setEnvIfUnset("PS2X_MC_ROOT", (options.dataDir / "memcards" / (stem.empty() ? "default" : stem) / "mc0").string());
        setEnvIfUnset("PS2X_INPUT_CONFIG", (options.dataDir / "ps2x_input.cfg").string());
        const std::filesystem::path mods = options.dataDir / "mods";
        if (std::filesystem::is_directory(mods, ec))
            setEnvIfUnset("PS2X_MODS", mods.string());
        if (std::filesystem::is_regular_file(mods / "story_en.tsv", ec))
            setEnvIfUnset("PS2X_STORY_EN", (mods / "story_en.tsv").string());
        const std::filesystem::path hdPacks = options.dataDir / "hd_packs";
        if (std::filesystem::is_directory(hdPacks, ec))
            setEnvIfUnset("PS2X_HD_PACK", hdPacks.string());
    }
}

#endif
