#include "iop_cdvd.h"
#include "cdvdfsv_protocol.h"

#include "../core/iop_cpu.h"
#include "../core/iop_kernel.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/disc_observer.h"
#include "ps2x/iop/mod_overlay.h"
#include "ps2x/iop/ps2_path.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps2x::iop
{
    static DiscReadObserver g_discReadObserver;
    void setDiscReadObserver(DiscReadObserver observer) { g_discReadObserver = std::move(observer); }
    bool hasDiscReadObserver() { return static_cast<bool>(g_discReadObserver); }
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kSectorSize = 2048u;
        constexpr uint32_t kPrimaryVolumeDescriptorLsn = 16u;
        constexpr uint32_t kVolumeDescriptorTerminatorLsn = 17u;
        constexpr uint32_t kFirstDirectoryLsn = 20u;
        constexpr uint32_t kCdvdErrorNone = 0u;
        constexpr uint32_t kCdvdErrorRead = 0x30u;
        constexpr uint32_t kCdvdTypePs2Dvd = 0x14u;
        constexpr uint32_t kCdvdReadyComplete = 2u;
        constexpr uint32_t kCdvdStatusPause = 0x0Au;
        constexpr uint32_t kCdvdInitExit = 5u;
        constexpr uint32_t kCdvdCallbackRead = 1u;
        constexpr uint32_t kCdvdCallbackSeek = 4u;
        constexpr uint32_t kCdvdInterruptReadyBits = 0x29u;
        constexpr uint32_t kEventFlagMulti = 2u;
        constexpr uint32_t kCdvdStreamTimeout = 5000u;
        constexpr uint32_t kCdvdSyncTimeout = 15000u;

        // File index of a CRI CVM container (header + plain ISO9660), built only when a disc read observer is installed (texture provenance).
        struct CvmIndexFile
        {
            std::string path;
            uint32_t lsn = 0u; // ISO sector inside the container
            uint32_t size = 0u;
        };

        struct CvmIndex
        {
            uint32_t headerSectors = 0u;
            std::vector<CvmIndexFile> files;
            bool built = false;

            bool build(const std::filesystem::path &cvm)
            {
                built = true;
                std::ifstream in(cvm, std::ios::binary);
                if (!in)
                    return false;
                std::vector<uint8_t> pvd(kSectorSize);
                uint32_t header = 0xFFFFFFFFu;
                for (const uint32_t candidate : {0x1800u, 0u, 0x800u, 0x1000u, 0x2000u})
                {
                    in.clear();
                    in.seekg(static_cast<std::streamoff>(candidate) + 16 * static_cast<std::streamoff>(kSectorSize));
                    in.read(reinterpret_cast<char *>(pvd.data()), kSectorSize);
                    if (in.gcount() == static_cast<std::streamsize>(kSectorSize) && pvd[0] == 1u && std::memcmp(pvd.data() + 1, "CD001", 5) == 0)
                    {
                        header = candidate;
                        break;
                    }
                }
                if (header == 0xFFFFFFFFu)
                    return false;
                headerSectors = header / kSectorSize;
                auto le32 = [](const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); };
                walk(in, header, le32(pvd.data() + 158), le32(pvd.data() + 166), std::string(), 0);
                std::sort(files.begin(), files.end(), [](const CvmIndexFile &a, const CvmIndexFile &c) { return a.lsn < c.lsn; });
                return !files.empty();
            }

        private:
            void walk(std::ifstream &in, uint32_t header, uint32_t lsn, uint32_t size, const std::string &prefix, int depth)
            {
                if (depth > 8 || size == 0u || size > (64u << 20))
                    return;
                std::vector<uint8_t> data(size);
                in.clear();
                in.seekg(static_cast<std::streamoff>(header) + static_cast<std::streamoff>(lsn) * kSectorSize);
                in.read(reinterpret_cast<char *>(data.data()), size);
                if (in.gcount() != static_cast<std::streamsize>(size))
                    return;
                auto le32 = [](const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); };
                size_t o = 0;
                while (o + 34 <= data.size())
                {
                    const uint32_t n = data[o];
                    if (n == 0u)
                    {
                        o = (o / kSectorSize + 1) * kSectorSize;
                        continue;
                    }
                    const uint32_t nameLen = data[o + 32];
                    if (o + 33 + nameLen > data.size())
                        break;
                    const uint32_t extent = le32(data.data() + o + 2), extentSize = le32(data.data() + o + 10);
                    const bool special = nameLen == 1u && (data[o + 33] == 0u || data[o + 33] == 1u);
                    if (!special)
                    {
                        std::string name(reinterpret_cast<const char *>(data.data() + o + 33), nameLen);
                        if (name.size() > 2 && name.compare(name.size() - 2, 2, ";1") == 0)
                            name.resize(name.size() - 2);
                        for (char &c : name)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        const std::string path = prefix.empty() ? name : prefix + "/" + name;
                        if (data[o + 25] & 2u)
                            walk(in, header, extent, extentSize, path, depth + 1);
                        else
                            files.push_back({path, extent, extentSize});
                    }
                    o += n;
                }
            }
        };

        uint32_t alignSectors(uint64_t bytes)
        {
            return static_cast<uint32_t>((bytes + kSectorSize - 1u) / kSectorSize);
        }

        void writeLe16(uint8_t *destination, uint16_t value)
        {
            destination[0] = static_cast<uint8_t>(value);
            destination[1] = static_cast<uint8_t>(value >> 8u);
        }

        void writeBe16(uint8_t *destination, uint16_t value)
        {
            destination[0] = static_cast<uint8_t>(value >> 8u);
            destination[1] = static_cast<uint8_t>(value);
        }

        void writeLe32(uint8_t *destination, uint32_t value)
        {
            for (uint32_t i = 0u; i < 4u; ++i)
                destination[i] = static_cast<uint8_t>(value >> (i * 8u));
        }

        void writeBe32(uint8_t *destination, uint32_t value)
        {
            for (uint32_t i = 0u; i < 4u; ++i)
                destination[i] = static_cast<uint8_t>(value >> ((3u - i) * 8u));
        }

        void writeBoth16(uint8_t *destination, uint16_t value)
        {
            writeLe16(destination, value);
            writeBe16(destination + 2u, value);
        }

        void writeBoth32(uint8_t *destination, uint32_t value)
        {
            writeLe32(destination, value);
            writeBe32(destination + 4u, value);
        }

        std::string isoName(const std::filesystem::path &path, bool directory)
        {
            std::string name = path.filename().string();
            for (char &character : name)
            {
                const unsigned char byte = static_cast<unsigned char>(character);
                character = byte < 0x80u ? static_cast<char>(std::toupper(byte)) : '_';
            }
            if (name.size() > 200u)
                name.resize(200u);
            if (!directory && name.find(';') == std::string::npos)
                name += ";1";
            return name;
        }

        size_t directoryRecordSize(size_t identifierSize)
        {
            const size_t unpadded = 33u + identifierSize;
            return unpadded + (unpadded & 1u);
        }

        uint32_t directoryBytesFor(const std::vector<size_t> &identifierSizes)
        {
            uint64_t cursor = 0u;
            for (const size_t identifierSize : identifierSizes)
            {
                const uint64_t recordSize = directoryRecordSize(identifierSize);
                const uint64_t sectorOffset = cursor % kSectorSize;
                if (sectorOffset + recordSize > kSectorSize)
                    cursor += kSectorSize - sectorOffset;
                cursor += recordSize;
            }
            return static_cast<uint32_t>(std::max<uint64_t>(kSectorSize, alignSectors(cursor) * kSectorSize));
        }

        std::string normalizedIsoComponent(std::string_view value)
        {
            std::string result(value);
            const size_t semicolon = result.rfind(';');
            if (semicolon != std::string::npos && semicolon + 1u < result.size() &&
                std::all_of(result.begin() + static_cast<std::ptrdiff_t>(semicolon + 1u), result.end(),
                            [](unsigned char ch)
                            { return std::isdigit(ch) != 0; }))
            {
                result.resize(semicolon);
            }
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char ch)
                           { return static_cast<char>(std::toupper(ch)); });
            return result;
        }

        size_t writeDirectoryRecord(uint8_t *destination,
                                    uint32_t lsn,
                                    uint32_t size,
                                    bool directory,
                                    const uint8_t *identifier,
                                    size_t identifierSize)
        {
            const size_t recordSize = directoryRecordSize(identifierSize);
            std::memset(destination, 0, recordSize);
            destination[0] = static_cast<uint8_t>(recordSize);
            writeBoth32(destination + 2u, lsn);
            writeBoth32(destination + 10u, size);
            destination[25] = directory ? 2u : 0u;
            writeBoth16(destination + 28u, 1u);
            destination[32] = static_cast<uint8_t>(identifierSize);
            std::memcpy(destination + 33u, identifier, identifierSize);
            return recordSize;
        }
    }

    class IopCdvd::Impl
    {
    public:
        struct Callback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
        };

        struct IsoNode
        {
            std::filesystem::path hostPath;
            std::string identifier;
            size_t parent = 0u;
            bool directory = false;
            uint32_t lsn = 0u;
            uint32_t size = 0u;
            uint32_t sectors = 0u;
            uint64_t handle = 0u;
            std::vector<size_t> children;
            std::shared_ptr<mods::CvmOverlay> cvm; // container with replaced files (see mod_overlay.h)
        };

        std::unordered_map<size_t, CvmIndex> cvmIndexes; // texture provenance (only filled when a disc read observer is installed)

        Impl(IopHost &hostRef, IopMemory &memoryRef, IopKernel &kernelRef)
            : host(hostRef), memory(memoryRef), kernel(kernelRef)
        {
        }

        ~Impl()
        {
            closeFiles();
        }

        void reset()
        {
            closeFiles();
            callback = {};
            device.reset();
            const auto image = host.hostPath(HostPathKind::CdImage);
            const auto root = host.hostPath(HostPathKind::CdRoot);
            sourceImage = image;
            sourceRoot = root;
            sourceSelected = false;
            imageChecked = imageValid = false;
            imageBytes = 0;
            imageSectors = 0;
            device.configureSource(!image.empty() ? CdvdDevice::Source::ImageCandidate :
                !root.empty() ? CdvdDevice::Source::DirectoryCandidate : CdvdDevice::Source::Unconfigured,
                !image.empty() ? image : root);
            device.prepare();
            currentLsn = 0u;
            lastError = kCdvdErrorNone;
            streamFlag = 0u;
            lastReadTimeout = 0u;
            interruptEventFlagId = 0;
            virtualIsoBuilt = false;
            virtualIsoValid = false;
            completionCallback.reset();
            nodes.clear();
            metadataSectors.clear();
            imageHandle = 0u;
        }

        CdvdDevice::Snapshot snapshot() const { return device.snapshot(); }

        CdvdDevice::Result control(CdvdControl operation, uint32_t argument)
        {
            if (operation == CdvdControl::Mmode) return device.setMode(argument);
            if (operation == CdvdControl::DiskReady) return device.diskReady(argument);
            auto result = device.init(argument);
            if (result.phase == CdvdDevice::Phase::Completed)
            {
                if (argument != kCdvdInitExit)
                {
                    callback = {};
                    completionCallback.reset();
                }
                lastError = kCdvdErrorNone;
            }
            return result;
        }

        RpcResult handleControlRpc(const RpcRequest &request)
        {
            if (request.sid == kCdvdfsvNcmdSid)
            {
                const auto state = device.snapshot();
                if (!state.prepared || state.activeOperations)
                {
                    host.log(LogLevel::Warning, "[CDVDFSV:HLE] NCMD function=" + std::to_string(request.function) +
                                                    " deferred: prepared=" + std::to_string(state.prepared) +
                                                    " active=" + std::to_string(state.activeOperations));
                    return RpcResult{true, false};
                }
                return handleNcmdRpc(request);
            }
            if (request.sid == 0x80000593u && request.function == 4u)
            {
                // S-command 4: sceCdGetError, {retres}. The synchronous model keeps the last error code.
                RpcResult result{};
                result.handled = true;
                if (request.receive.size < 4u || request.receive.address == 0u) return result;
                const uint32_t error = static_cast<uint32_t>(lastError);
                const std::array<uint8_t, 4> reply{uint8_t(error), uint8_t(error >> 8), uint8_t(error >> 16), uint8_t(error >> 24)};
                if (!host.writeGuest(request.receive.address, reply.data(), reply.size())) return result;
                result.resultAddress = request.receive.address;
                result.packetCompleted = true;
                host.log(LogLevel::Debug, "[CDVDFSV:HLE] SCMD GetError -> " + std::to_string(error));
                return result;
            }
            if (request.sid == kCdvdfsvSearchSid)
            {
                const auto state = device.snapshot();
                if (!state.prepared || state.activeOperations) return RpcResult{true, false};
                return handleSearchRpc(request);
            }
            return handleCdvdfsv(host, request, [&](CdvdControl op, uint32_t arg) {
                const auto state = device.snapshot();
                // No deferred buffers are owned by this profile. Reject before
                // creating a waiter if the synchronous invariant stops holding.
                if (!state.prepared || state.activeOperations) return CdvdDevice::Result{};
                return control(op, arg);
            });
        }

        bool dispatchImport(uint16_t ordinal, IopCpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            const uint32_t a1 = cpu.gpr[5];
            const uint32_t a2 = cpu.gpr[6];

            switch (ordinal)
            {
            case 4: // sceCdInit: synchronous backend has no outstanding work at import boundaries
            {
                const auto result = control(CdvdControl::Init, a0);
                if (result.phase != CdvdDevice::Phase::Completed) return false;
                cpu.gpr[2] = static_cast<uint32_t>(*result.value);
                return true;
            }

            case 5: // sceCdStandby
                cpu.gpr[2] = 1u;
                return true;

            case 6: // sceCdRead
                if (readSectors(a0, a1, a2))
                {
                    signalCommandComplete();
                    if (callback.address != 0u)
                    {
                        completionCallback = CompletionCallback{
                            callback.address,
                            callback.gp,
                            kCdvdCallbackRead,
                        };
                    }
                    cpu.gpr[2] = 1u;
                }
                else
                    cpu.gpr[2] = 0u;
                return true;

            case 7: // sceCdSeek
                currentLsn = a0;
                lastError = kCdvdErrorNone;
                signalCommandComplete();
                if (callback.address != 0u)
                {
                    completionCallback = CompletionCallback{
                        callback.address,
                        callback.gp,
                        kCdvdCallbackSeek,
                    };
                }
                cpu.gpr[2] = 1u;
                return true;

            case 8: // sceCdGetError
                cpu.gpr[2] = lastError;
                return true;

            case 10: // sceCdSearchFile
                cpu.gpr[2] = searchFile(a0, a1) ? 1u : 0u;
                return true;

            case 11: // sceCdSync
                cpu.gpr[2] = 0u;
                return true;

            case 12: // sceCdGetDiskType
                cpu.gpr[2] = kCdvdTypePs2Dvd;
                return true;

            case 13: // raw mode8 and unsupported wait profiles are not fabricated
            {
                const auto result = control(CdvdControl::DiskReady, a0);
                if (result.phase != CdvdDevice::Phase::Completed) return false;
                cpu.gpr[2] = static_cast<uint32_t>(*result.value);
                return true;
            }

            case 28: // sceCdStatus
                cpu.gpr[2] = kCdvdStatusPause;
                return true;

            case 37: // sceCdCallback
            {
                const uint32_t previous = callback.address;
                callback = {a0, cpu.gpr[28]};
                cpu.gpr[2] = previous;
                return true;
            }

            case 50: // sceCdSC
            {
                const int32_t code = static_cast<int32_t>(a0);
                switch (code)
                {
                case -23: // Translate a logical sector for a dual-layer disc.
                    // The host image exposes one continuous LSN space, so no layer offset is required.
                    cpu.gpr[2] = a1 != 0u ? memory.read32(a1) : 0u;
                    return true;
                case -18:
                    lastReadTimeout = a1 != 0u ? memory.read32(a1) : 0u;
                    cpu.gpr[2] = 0u;
                    return true;
                case -17:
                    cpu.gpr[2] = kCdvdStreamTimeout;
                    return true;
                case -15:
                    cpu.gpr[2] = kCdvdSyncTimeout;
                    return true;
                case -11:
                    cpu.gpr[2] = static_cast<uint32_t>(ensureInterruptEventFlag());
                    return true;
                case -9:
                    cpu.gpr[2] = kCdvdmanVersion;
                    return true;
                case -2:
                    lastError = a1 != 0u ? memory.read8(a1) : kCdvdErrorNone;
                    cpu.gpr[2] = lastError;
                    return true;
                case -1:
                case 0:
                case 1:
                case 2:
                    if (a1 != 0u)
                        memory.write32(a1, lastError & 0xFFu);
                    if (code != -1)
                        streamFlag = static_cast<uint32_t>(code);
                    cpu.gpr[2] = streamFlag;
                    return true;
                default:
                    // sceCdSC is intentionally extensible; unsupported controls are no-ops in cdvdman.
                    cpu.gpr[2] = 0u;
                    return true;
                }
            }

            case 75: // sceCdMmode
            {
                const auto result = control(CdvdControl::Mmode, a0);
                if (result.phase != CdvdDevice::Phase::Completed) return false;
                cpu.gpr[2] = static_cast<uint32_t>(*result.value);
                return true;
            }

            default:
                return false;
            }
        }

        std::optional<CompletionCallback> takeCompletionCallback() noexcept
        {
            std::optional<CompletionCallback> result = completionCallback;
            completionCallback.reset();
            return result;
        }

    private:
        int ensureInterruptEventFlag()
        {
            if (interruptEventFlagId == 0)
            {
                interruptEventFlagId = kernel.createInternalEventFlag(
                    kEventFlagMulti, 0u, kCdvdInterruptReadyBits);
            }
            return interruptEventFlagId;
        }

        void signalCommandComplete()
        {
            if (interruptEventFlagId != 0)
                (void)kernel.setInternalEventFlag(interruptEventFlagId, kCdvdInterruptReadyBits);
        }

        void closeFiles()
        {
            if (imageHandle != 0u)
                host.closeHostFile(imageHandle);
            imageHandle = 0u;
            for (IsoNode &node : nodes)
            {
                if (node.handle != 0u)
                    host.closeHostFile(node.handle);
                node.handle = 0u;
            }
        }

        // Resource replacement for one file of the virtual disc: DISC/<path> swaps the host file (any size, the layout is
        // computed afterwards); a CVM container with files in <mods>/<NAME>/ is presented through a CvmOverlay.
        void applyMods(IsoNode &node, const std::string &relative)
        {
            if (modRoot.empty())
                return;
            const std::string logical = mods::normalizeLogicalPath(relative.empty() ? node.identifier : relative + "/" + node.identifier);
            const auto logFn = [this](const std::string &text) { host.log(LogLevel::Info, text); };
            if (const auto *disc = modRoot.files("DISC"))
            {
                const auto found = disc->find(logical);
                std::error_code error;
                if (found != disc->end())
                {
                    const uint64_t size = std::filesystem::file_size(found->second, error);
                    if (!error && size <= std::numeric_limits<uint32_t>::max())
                    {
                        logFn("[mods] DISC/" + logical + ": " + std::to_string(node.size) + " -> " + std::to_string(size) + " bytes <- " +
                              found->second.string());
                        node.hostPath = found->second;
                        node.size = static_cast<uint32_t>(size);
                    }
                }
            }
            if (relative.empty())
            {
                const std::string stem = logical.size() > 4 && logical.compare(logical.size() - 4, 4, ".CVM") == 0
                                             ? logical.substr(0, logical.size() - 4)
                                             : std::string();
                const auto *files = stem.empty() ? nullptr : modRoot.files(stem);
                if (files != nullptr)
                {
                    const std::filesystem::path original = node.hostPath;
                    auto stream = std::make_shared<std::ifstream>(original, std::ios::binary);
                    auto reader = [stream](uint64_t offset, void *destination, size_t size)
                    {
                        if (!*stream)
                            return false;
                        stream->clear();
                        stream->seekg(static_cast<std::streamoff>(offset));
                        stream->read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
                        return static_cast<size_t>(stream->gcount()) == size;
                    };
                    node.cvm = mods::CvmOverlay::build(stem, node.size, reader, *files, logFn);
                    if (node.cvm)
                        node.size = static_cast<uint32_t>(std::min<uint64_t>(node.cvm->size(), std::numeric_limits<uint32_t>::max()));
                }
            }
        }

        bool addDirectory(size_t parent, const std::filesystem::path &path, const std::string &relative = std::string())
        {
            std::error_code error;
            std::vector<std::filesystem::directory_entry> entries;
            for (
                std::filesystem::directory_iterator iterator(path, std::filesystem::directory_options::skip_permission_denied, error),
                end;
                !error && iterator != end;
                iterator.increment(error))
            {
                const std::filesystem::directory_entry &entry = *iterator;
                if (entry.is_symlink(error))
                {
                    error.clear();
                    continue;
                }
                error.clear();
                if (entry.is_directory(error) || entry.is_regular_file(error))
                    entries.push_back(entry);
                error.clear();
            }

            std::sort(entries.begin(), entries.end(),
                      [](const auto &lhs, const auto &rhs)
                      {
                          return isoName(lhs.path(), lhs.is_directory()) < isoName(rhs.path(), rhs.is_directory());
                      });

            for (const auto &entry : entries)
            {
                error.clear();
                const bool directory = entry.is_directory(error);
                if (error)
                    continue;
                IsoNode node;
                node.hostPath = entry.path();
                node.identifier = isoName(entry.path(), directory);
                node.parent = parent;
                node.directory = directory;
                if (!directory)
                {
                    const uint64_t fileSize = entry.file_size(error);
                    if (error)
                        continue;
                    node.size = static_cast<uint32_t>(std::min<uint64_t>(fileSize, std::numeric_limits<uint32_t>::max()));
                    applyMods(node, relative);
                }
                const size_t index = nodes.size();
                nodes.push_back(std::move(node));
                nodes[parent].children.push_back(index);
                if (directory &&
                    !addDirectory(index, entry.path(),
                                  relative.empty() ? mods::normalizeLogicalPath(entry.path().filename().string())
                                                   : relative + "/" + mods::normalizeLogicalPath(entry.path().filename().string())))
                    return false;
            }
            return true;
        }

        bool buildVirtualIso()
        {
            if (virtualIsoBuilt)
                return virtualIsoValid;
            virtualIsoBuilt = true;

            const std::string &rootValue = sourceRoot;
            if (rootValue.empty())
                return false;
            const std::filesystem::path rootPath(rootValue);
            std::error_code error;
            if (!std::filesystem::is_directory(rootPath, error) || error)
                return false;

            modRoot = mods::ModRoot();
            if (const std::string modPath = host.hostPath(HostPathKind::ModRoot); !modPath.empty())
            {
                if (modRoot.open(modPath, [this](const std::string &text) { host.log(LogLevel::Info, text); }))
                    host.log(LogLevel::Info, "[mods] folder " + modPath + ": " + std::to_string(modRoot.fileCount()) + " replacement file(s)");
                else
                    host.log(LogLevel::Info, "[mods] folder " + modPath + " not found, no replacements");
            }

            nodes.clear();
            IsoNode root;
            root.hostPath = rootPath;
            root.identifier.clear();
            root.parent = 0u;
            root.directory = true;
            nodes.push_back(std::move(root));
            if (!addDirectory(0u, rootPath))
                return false;

            for (IsoNode &node : nodes)
            {
                if (!node.directory)
                    continue;
                std::vector<size_t> identifierSizes = {1u, 1u};
                identifierSizes.reserve(node.children.size() + 2u);
                for (const size_t child : node.children)
                    identifierSizes.push_back(nodes[child].identifier.size());
                node.size = directoryBytesFor(identifierSizes);
                node.sectors = node.size / kSectorSize;
            }

            uint32_t cursor = kFirstDirectoryLsn;
            for (IsoNode &node : nodes)
            {
                if (!node.directory)
                    continue;
                node.lsn = cursor;
                cursor += node.sectors;
            }
            for (IsoNode &node : nodes)
            {
                if (node.directory)
                    continue;
                node.lsn = cursor;
                node.sectors = alignSectors(node.size);
                cursor += node.sectors;
            }
            volumeSectors = std::max<uint32_t>(cursor, 32u);

            std::array<uint8_t, kSectorSize> primary{};
            primary[0] = 1u;
            std::memcpy(primary.data() + 1u, "CD001", 5u);
            primary[6] = 1u;
            std::memset(primary.data() + 8u, ' ', 32u);
            std::memcpy(primary.data() + 8u, "PS2XRECOMP", 10u);
            std::memset(primary.data() + 40u, ' ', 32u);
            std::memcpy(primary.data() + 40u, "PS2X VIRTUAL DISC", 17u);
            writeBoth32(primary.data() + 80u, volumeSectors);
            writeBoth16(primary.data() + 120u, 1u);
            writeBoth16(primary.data() + 124u, 1u);
            writeBoth16(primary.data() + 128u, static_cast<uint16_t>(kSectorSize));
            const uint8_t rootIdentifier = 0u;
            (void)writeDirectoryRecord(primary.data() + 156u, nodes[0].lsn, nodes[0].size, true, &rootIdentifier, 1u);
            primary[881] = 1u;
            metadataSectors[kPrimaryVolumeDescriptorLsn] = primary;

            std::array<uint8_t, kSectorSize> terminator{};
            terminator[0] = 255u;
            std::memcpy(terminator.data() + 1u, "CD001", 5u);
            terminator[6] = 1u;
            metadataSectors[kVolumeDescriptorTerminatorLsn] = terminator;

            for (size_t nodeIndex = 0u; nodeIndex < nodes.size(); ++nodeIndex)
            {
                const IsoNode &node = nodes[nodeIndex];
                if (!node.directory)
                    continue;
                std::vector<uint8_t> bytes(node.size, 0u);
                size_t offset = 0u;
                const auto appendRecord = [&](const IsoNode &entry, const uint8_t *identifier, size_t identifierSize)
                {
                    const size_t recordSize = directoryRecordSize(identifierSize);
                    const size_t sectorOffset = offset % kSectorSize;
                    if (sectorOffset + recordSize > kSectorSize)
                        offset += kSectorSize - sectorOffset;
                    offset += writeDirectoryRecord(bytes.data() + offset,
                                                   entry.lsn,
                                                   entry.size,
                                                   entry.directory,
                                                   identifier,
                                                   identifierSize);
                };
                const uint8_t selfIdentifier = 0u;
                const uint8_t parentIdentifier = 1u;
                appendRecord(node, &selfIdentifier, 1u);
                appendRecord(nodes[node.parent], &parentIdentifier, 1u);
                for (const size_t childIndex : node.children)
                {
                    const IsoNode &child = nodes[childIndex];
                    appendRecord(child, reinterpret_cast<const uint8_t *>(child.identifier.data()), child.identifier.size());
                }
                for (uint32_t sector = 0u; sector < node.sectors; ++sector)
                {
                    std::array<uint8_t, kSectorSize> contents{};
                    std::memcpy(contents.data(), bytes.data() + sector * kSectorSize, kSectorSize);
                    metadataSectors[node.lsn + sector] = contents;
                }
            }

            virtualIsoValid = true;
            return true;
        }

        IsoNode *findVirtualIsoNode(std::string_view guestPath)
        {
            if (!buildVirtualIso())
                return nullptr;

            const ParsedPs2Path parsed = parsePs2Path(guestPath);
            if (!parsed || parsed.device != Ps2PathDevice::Cdrom)
                return nullptr;

            size_t current = 0u;
            size_t begin = 0u;
            while (begin <= parsed.path.size())
            {
                const size_t end = parsed.path.find('/', begin);
                const size_t length = (end == std::string::npos) ? parsed.path.size() - begin : end - begin;
                const std::string_view component(parsed.path.data() + begin, length);
                begin = (end == std::string::npos) ? parsed.path.size() + 1u : end + 1u;

                if (component.empty() || component == ".")
                    continue;
                if (component == "..")
                    return nullptr;

                const std::string wanted = normalizedIsoComponent(component);
                const auto child = std::find_if(nodes[current].children.begin(), nodes[current].children.end(),
                                                [&](size_t childIndex)
                                                {
                                                    return normalizedIsoComponent(nodes[childIndex].identifier) == wanted;
                                                });
                if (child == nodes[current].children.end())
                    return nullptr;
                current = *child;
            }
            return &nodes[current];
        }

        // One source per reset generation. A configured image never falls back
        // to the unrelated virtual directory layout, including on IO failure.
        struct ImageEntry { uint32_t lsn = 0, size = 0; bool directory = false; std::string name; };
        void selectSource()
        {
            if (sourceSelected) return;
            sourceSelected = true;
            sourceImage = host.hostPath(HostPathKind::CdImage);
            sourceRoot = host.hostPath(HostPathKind::CdRoot);
            device.configureSource(!sourceImage.empty() ? CdvdDevice::Source::ImageCandidate :
                !sourceRoot.empty() ? CdvdDevice::Source::DirectoryCandidate : CdvdDevice::Source::Unconfigured,
                !sourceImage.empty() ? sourceImage : sourceRoot);
        }
        static bool bothNumber(const uint8_t *p, size_t width, uint32_t &value)
        {
            uint32_t le = 0, be = 0;
            for (size_t i = 0; i < width; ++i)
            {
                le |= uint32_t(p[i]) << (8 * i);
                be = (be << 8) | p[width + i];
            }
            value = le;
            return le == be;
        }
        bool imageRead(uint64_t offset, void *destination, size_t size)
        {
            uint64_t currentSize = 0;
            if (!imageHandle || !host.hostFileSize(imageHandle, currentSize) || currentSize != imageBytes ||
                offset > imageBytes || size > imageBytes - offset)
                return false;
            size_t bytesRead = 0;
            return host.readHostFile(imageHandle, offset, destination, size, bytesRead) && bytesRead == size;
        }
        bool imageRecord(const uint8_t *p, size_t available, ImageEntry &entry)
        {
            if (available < 34 || p[0] < 34 || p[0] > available || !p[32] ||
                directoryRecordSize(p[32]) > p[0] || p[1] || p[26] || p[27] ||
                (p[25] & ~3u)) // supported profile: contiguous single extent, no XAR/interleave
                return false;
            uint32_t sequence = 0;
            if (!bothNumber(p + 2, 4, entry.lsn) || !bothNumber(p + 10, 4, entry.size) ||
                !bothNumber(p + 28, 2, sequence) || sequence != 1 ||
                entry.lsn >= imageSectors || uint64_t(entry.lsn) + alignSectors(entry.size) > imageSectors)
                return false;
            entry.directory = (p[25] & 2) != 0;
            if (entry.directory && (!entry.size || entry.size % kSectorSize)) return false;
            entry.name.assign(reinterpret_cast<const char *>(p + 33), p[32]);
            return true;
        }
        bool prepareImage()
        {
            if (imageChecked) return imageValid;
            imageChecked = true;
            imageHandle = host.openHostFile(sourceImage);
            if (!imageHandle || !host.hostFileSize(imageHandle, imageBytes) || imageBytes % kSectorSize)
                return false;
            std::array<uint8_t, kSectorSize> descriptor{};
            bool primary = false;
            // Bounded descriptor traversal; unsupported/malformed images fail closed.
            for (uint32_t lsn = 16; lsn < 272; ++lsn)
            {
                if ((primary && lsn >= imageSectors) ||
                    !imageRead(uint64_t(lsn) * kSectorSize, descriptor.data(), descriptor.size()) ||
                    std::memcmp(descriptor.data() + 1, "CD001", 5) || descriptor[6] != 1)
                    return false;
                if (descriptor[0] == 255) return imageValid = primary;
                if (descriptor[0] != 1) continue;
                if (primary) return false;
                uint32_t blockSize = 0, volumes = 0, sequence = 0;
                if (!bothNumber(descriptor.data() + 80, 4, imageSectors) || imageSectors <= lsn ||
                    uint64_t(imageSectors) * kSectorSize > imageBytes ||
                    !bothNumber(descriptor.data() + 128, 2, blockSize) || blockSize != kSectorSize ||
                    !bothNumber(descriptor.data() + 120, 2, volumes) || volumes != 1 ||
                    !bothNumber(descriptor.data() + 124, 2, sequence) || sequence != 1 ||
                    !imageRecord(descriptor.data() + 156, kSectorSize - 156, imageRoot) ||
                    !imageRoot.directory || imageRoot.name != std::string(1, '\0'))
                    return false;
                primary = true;
            }
            return false;
        }
        bool searchImage(std::string_view path, ImageEntry &result)
        {
            if (!prepareImage()) return false;
            const auto parsed = parsePs2Path(path);
            if (!parsed || parsed.device != Ps2PathDevice::Cdrom) return false;
            ImageEntry current = imageRoot;
            size_t begin = 0;
            while (begin < parsed.path.size())
            {
                const auto end = parsed.path.find('/', begin);
                const auto part = parsed.path.substr(begin, end == std::string::npos ? end : end - begin);
                begin = end == std::string::npos ? parsed.path.size() : end + 1;
                if (part.empty() || part == ".") continue;
                if (part == ".." || !current.directory || current.size > (1u << 20)) return false;
                std::vector<uint8_t> bytes(current.size);
                if (!imageRead(uint64_t(current.lsn) * kSectorSize, bytes.data(), bytes.size())) return false;
                bool found = false;
                ImageEntry match;
                for (size_t offset = 0; offset < bytes.size();)
                {
                    if (!bytes[offset]) { offset = (offset / kSectorSize + 1) * kSectorSize; continue; }
                    ImageEntry entry;
                    const size_t available = std::min(bytes.size() - offset, kSectorSize - offset % kSectorSize);
                    if (!imageRecord(bytes.data() + offset, available, entry)) return false;
                    if (normalizedIsoComponent(entry.name) == normalizedIsoComponent(part))
                    {
                        if (found) return false; // ambiguous normalized names/version extents
                        match = std::move(entry); found = true;
                    }
                    offset += bytes[offset];
                }
                if (!found) return false;
                current = std::move(match);
            }
            result = std::move(current);
            return true;
        }

        // sceCdlFILE record (lsn, size, name[16], date/flags[8]) of a CD path, if it exists.
        bool searchFileRecord(const std::string &guestPath, std::array<uint8_t, 32u> &result)
        {
            selectSource();
            ImageEntry entry;
            if (!sourceImage.empty())
            {
                if (!searchImage(guestPath, entry)) return false;
            }
            else
            {
                IsoNode *node = findVirtualIsoNode(guestPath);
                if (!node) return false;
                entry = {node->lsn, node->size, node->directory, node->identifier};
            }

            result = {};
            writeLe32(result.data(), entry.lsn);
            writeLe32(result.data() + 4u, entry.size);
            const std::string leaf = normalizedIsoComponent(entry.name);
            std::memcpy(result.data() + 8u, leaf.data(), std::min<size_t>(16u, leaf.size()));
            result[24u] = entry.directory ? 2u : 0u;
            return true;
        }

        bool searchFile(uint32_t resultAddress, uint32_t nameAddress)
        {
            if (resultAddress == 0u || nameAddress == 0u)
                return false;

            std::array<uint8_t, 32u> result{};
            if (!searchFileRecord(memory.readString(nameAddress, 1024u), result)) return false;
            return memory.writeRam(resultAddress, result.data(), result.size());
        }

        // CDVDFSV search-file RPC (PS2SDK cdvdfsv.c cbrpc_rpc4_fscall). The request is a sceCdlFILE
        // followed by [file attributes] path[256] eedest [layer]; the server fills the sceCdlFILE
        // (+ the attributes word) at `eedest` in EE memory and answers {found, 0, 0, 0}.
        RpcResult handleSearchRpc(const RpcRequest &request)
        {
            RpcResult result{};
            result.handled = true;
            const uint32_t size = request.send.size;
            if (request.function != 0u || (size != 0x124u && size != 0x128u && size != 0x12Cu) ||
                request.receive.size < 4u || request.send.address == 0u || request.receive.address == 0u)
                return result;
            std::vector<uint8_t> in(size);
            if (!host.readGuest(request.send.address, in.data(), in.size())) return result;
            const size_t pathOffset = size == 0x124u ? 0x20u : 0x24u;
            const size_t destOffset = pathOffset + 256u;
            const uint32_t destination = uint32_t(in[destOffset]) | (uint32_t(in[destOffset + 1]) << 8) |
                                         (uint32_t(in[destOffset + 2]) << 16) | (uint32_t(in[destOffset + 3]) << 24);
            size_t length = 0u;
            while (length < 256u && in[pathOffset + length] != 0u) ++length;
            std::string path(reinterpret_cast<const char *>(in.data() + pathOffset), length);
            if (parsePs2Path(path).device == Ps2PathDevice::Invalid)
                path = "cdrom0:" + path; // sceCdSearchFile takes device-less paths

            std::array<uint8_t, 32u> record{};
            std::memcpy(record.data(), in.data(), record.size());
            const bool found = searchFileRecord(path, record);
            host.log(LogLevel::Info, std::string("[CDVDFSV:HLE] search '") + path + (found ? "' -> lsn " : "' -> not found") +
                                         (found ? std::to_string(uint32_t(record[0]) | (uint32_t(record[1]) << 8) | (uint32_t(record[2]) << 16) | (uint32_t(record[3]) << 24)) : std::string()));
            // The DMA to the EE carries the record (and the attributes word of the newer layouts).
            const size_t dmaSize = size == 0x124u ? 32u : 36u;
            std::vector<uint8_t> dma(in.begin(), in.begin() + dmaSize);
            std::memcpy(dma.data(), record.data(), record.size());
            if (destination && !host.writeGuest(destination, dma.data(), dma.size())) return result;
            std::vector<uint8_t> reply(std::min<size_t>(request.receive.size, 16u), 0u);
            reply[0] = found ? 1u : 0u;
            if (!host.writeGuest(request.receive.address, reply.data(), reply.size())) return result;
            result.resultAddress = request.receive.address;
            result.packetCompleted = true;
            return result;
        }

        // CDVDFSV N-command service (PS2SDK ncmd.c / cdvdfsv.c). Only sceCdRead (function 1) is
        // served: {lsn, sectors, EE buffer, mode, EE unaligned-data block, -}. The sectors are written
        // straight into the EE buffer, so the client's unaligned head/tail block (b1len, b2len) is
        // cleared. The RPC is non-blocking: its END packet is the read-complete notification.
        RpcResult handleNcmdRpc(const RpcRequest &request)
        {
            RpcResult result{};
            result.handled = true;
            auto unsupported = [&](const char *why) {
                std::string head;
                std::vector<uint8_t> raw(std::min<uint32_t>(request.send.size, 48u));
                if (request.send.address && host.readGuest(request.send.address, raw.data(), raw.size()))
                {
                    static const char hex[] = "0123456789abcdef";
                    for (uint8_t b : raw) { head += hex[b >> 4]; head += hex[b & 15]; }
                }
                host.log(LogLevel::Warning, "[CDVDFSV:HLE] unsupported NCMD function=" + std::to_string(request.function) +
                                                " send=" + std::to_string(request.send.size) + " receive=" + std::to_string(request.receive.size) +
                                                " (" + why + ") head=" + head);
                return result;
            };
            // Drive commands whose only effect here is their status answer {retres}: DiskReady (14)
            // reports 2 (complete) when a source is configured, otherwise 6 (not ready); Seek (5),
            // Standby (6), Stop (7) and Pause (8) are accepted (1). The non-blocking ones signal their
            // completion through the RPC's END packet.
            if (request.function == 14u || (request.function >= 5u && request.function <= 8u))
            {
                selectSource();
                uint32_t answer = 1u;
                if (request.function == 14u)
                    answer = (!sourceImage.empty() || !sourceRoot.empty()) ? 2u : 6u;
                if (request.receive.size >= 4u)
                {
                    std::vector<uint8_t> reply(std::min<size_t>(request.receive.size, 16u), 0u);
                    reply[0] = uint8_t(answer);
                    if (request.receive.address == 0u || !host.writeGuest(request.receive.address, reply.data(), reply.size()))
                        return result;
                    result.resultAddress = request.receive.address;
                }
                host.log(LogLevel::Debug, "[CDVDFSV:HLE] NCMD " + std::to_string(request.function) + " -> " + std::to_string(answer));
                result.packetCompleted = true;
                return result;
            }
            if (request.function != 1u || request.send.size < 20u || request.send.address == 0u)
                return unsupported("function");
            std::vector<uint8_t> in(request.send.size);
            if (!host.readGuest(request.send.address, in.data(), in.size())) return unsupported("arguments");
            auto word = [&](size_t at) { return uint32_t(in[at]) | (uint32_t(in[at + 1]) << 8) | (uint32_t(in[at + 2]) << 16) | (uint32_t(in[at + 3]) << 24); };
            const uint32_t lsn = word(0), sectors = word(4), destination = word(8), mode = word(12), unaligned = word(16);
            if ((mode >> 16) & 0xFFu) return unsupported("non-2048 data pattern");
            struct WorkGuard
            {
                CdvdDevice &device;
                CdvdDevice::Token token;
                ~WorkGuard() { device.finish(token); }
            } guard{device, device.begin(CdvdDevice::Work::Command)};
            std::vector<uint8_t> bytes;
            if (!destination || !readSectorBytes(lsn, sectors, bytes) || !host.writeGuest(destination, bytes.data(), bytes.size()))
            {
                lastError = kCdvdErrorRead;
                return unsupported("read failed");
            }
            if (unaligned)
            {
                const std::array<uint8_t, 8> none{};
                if (!host.writeGuest(unaligned, none.data(), none.size())) return result;
            }
            lastError = kCdvdErrorNone;
            if (hasDiscReadObserver())
            {
                DiscRead read;
                read.lsn = lsn;
                read.sectors = sectors;
                read.destination = destination;
                read.spans = describeSpans(lsn, sectors);
                g_discReadObserver(read);
            }
            host.log(LogLevel::Debug, "[CDVDFSV:HLE] read lsn " + std::to_string(lsn) + " x" + std::to_string(sectors));
            result.packetCompleted = true;
            // The data is in the EE buffer already, but the real drive delivers it over time and only then completes the
            // non-blocking read: a seek when the head has to move, then the sector transfer time. Games rely on this
            // (e.g. a stream's first prefetch must not look complete before the player has even started it).
            // PS2X_CD_SECTOR_US / PS2X_CD_SEEK_MS override the model (0 and 0 = instantaneous reads).
            static const double sectorMicros = []() { const char *v = std::getenv("PS2X_CD_SECTOR_US"); return v ? std::atof(v) : 350.0; }();
            static const double seekMillis = []() { const char *v = std::getenv("PS2X_CD_SEEK_MS"); return v ? std::atof(v) : 15.0; }();
            constexpr double kEeCyclesPerMicro = 294.912;
            double micros = sectorMicros * static_cast<double>(sectors);
            const bool seeked = lsn != ncmdNextLsn;
            if (seeked)
                micros += seekMillis * 1000.0;
            if (static const bool cdLog = std::getenv("PS2X_CD_LOG") != nullptr; cdLog)
                std::fprintf(stderr, "[cd] t=%lld lsn=%u sectors=%u dest=%x seek=%d delayUs=%.0f\n",
                             static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 100000000000ll),
                             lsn, sectors, destination, seeked ? 1 : 0, micros);
            ncmdNextLsn = lsn + sectors;
            result.completionDelayEeCycles = static_cast<uint64_t>(micros * kEeCyclesPerMicro);
            return result;
        }

        IsoNode *fileForSector(uint32_t lsn)
        {
            for (IsoNode &node : nodes)
            {
                if (!node.directory && lsn >= node.lsn && lsn < node.lsn + node.sectors)
                    return &node;
            }
            return nullptr;
        }

        // Texture provenance: which file(s) a read of [lsn, lsn + sectors) covers (upper case logical paths, "ST/..." inside ST.CVM, "DISC/..." for root files).
        std::string nodePath(size_t index) const
        {
            std::string path;
            for (size_t at = index; at < nodes.size() && at != 0u; at = nodes[at].parent)
            {
                std::string part = nodes[at].identifier;
                if (part.size() > 2 && part.compare(part.size() - 2, 2, ";1") == 0)
                    part.resize(part.size() - 2);
                for (char &c : part)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                path = path.empty() ? part : part + "/" + path;
            }
            return path;
        }

        std::vector<DiscSpan> describeSpans(uint32_t lsn, uint32_t sectors)
        {
            std::vector<DiscSpan> spans;
            const IsoNode *node = fileForSector(lsn);
            if (!node)
                return spans;
            const size_t nodeIndex = static_cast<size_t>(node - nodes.data());
            const std::string nodeName = nodePath(nodeIndex);
            const bool isCvm = nodeName.size() > 4 && nodeName.compare(nodeName.size() - 4, 4, ".CVM") == 0;
            if (!isCvm)
            {
                DiscSpan span;
                span.path = "DISC/" + nodeName;
                span.fileOffset = static_cast<uint64_t>(lsn - node->lsn) * kSectorSize;
                span.bytes = sectors * kSectorSize;
                spans.push_back(span);
                return spans;
            }
            CvmIndex &index = cvmIndexes[nodeIndex];
            if (!index.built)
            {
                // Index the original container; files replaced by mods are moved to the extent the game is served (mod_overlay).
                std::filesystem::path original = node->hostPath;
                if (!index.build(original))
                    std::fprintf(stderr, "[texprov] could not index %s\n", original.string().c_str());
                if (node->cvm)
                {
                    for (const auto &replacement : node->cvm->replacements())
                        for (CvmIndexFile &file : index.files)
                            if (file.path == replacement.path)
                            {
                                file.lsn = replacement.lsn;
                                file.size = replacement.size;
                            }
                    std::sort(index.files.begin(), index.files.end(), [](const CvmIndexFile &a, const CvmIndexFile &c) { return a.lsn < c.lsn; });
                }
            }
            const std::string stem = nodeName.substr(0, nodeName.size() - 4);
            const uint64_t readBegin = static_cast<uint64_t>(lsn) * kSectorSize;
            const uint64_t readEnd = readBegin + static_cast<uint64_t>(sectors) * kSectorSize;
            for (const CvmIndexFile &file : index.files)
            {
                const uint64_t begin = static_cast<uint64_t>(node->lsn + index.headerSectors + file.lsn) * kSectorSize;
                const uint64_t end = begin + file.size;
                if (end <= readBegin || begin >= readEnd)
                    continue;
                const uint64_t from = std::max(begin, readBegin), to = std::min(end, readEnd);
                DiscSpan span;
                span.path = stem + "/" + file.path;
                span.fileOffset = from - begin;
                span.bytes = static_cast<uint32_t>(to - from);
                span.bufferOffset = static_cast<uint32_t>(from - readBegin);
                spans.push_back(span);
            }
            return spans;
        }

        bool readVirtualSector(uint32_t lsn, uint8_t *destination)
        {
            const auto metadata = metadataSectors.find(lsn);
            if (metadata != metadataSectors.end())
            {
                std::memcpy(destination, metadata->second.data(), kSectorSize);
                return true;
            }

            IsoNode *node = fileForSector(lsn);
            if (!node)
            {
                std::memset(destination, 0, kSectorSize);
                return lsn < volumeSectors;
            }
            if (node->cvm)
                return node->cvm->readSector(lsn - node->lsn, destination);
            if (node->handle == 0u)
                node->handle = host.openHostFile(node->hostPath.string());
            if (node->handle == 0u)
                return false;

            std::memset(destination, 0, kSectorSize);
            const uint64_t offset = static_cast<uint64_t>(lsn - node->lsn) * kSectorSize;
            const size_t wanted = static_cast<size_t>(std::min<uint64_t>(kSectorSize, static_cast<uint64_t>(node->size) - offset));
            size_t bytesRead = 0u;
            return host.readHostFile(node->handle, offset, destination, wanted, bytesRead) && bytesRead == wanted;
        }

        // Reads whole 2048 byte sectors from the selected source (image or virtual directory).
        // Callers account for the work in the device model through their own WorkGuard.
        bool readSectorBytes(uint32_t lsn, uint32_t sectors, std::vector<uint8_t> &bytes)
        {
            const uint64_t byteCount64 = static_cast<uint64_t>(sectors) * kSectorSize;
            if (byteCount64 > IopMemory::RamSize)
                return false;
            const size_t byteCount = static_cast<size_t>(byteCount64);
            bytes.assign(byteCount, 0u);

            bool read = false;
            selectSource();
            if (!sourceImage.empty())
            {
                read = prepareImage() && uint64_t(lsn) + sectors <= imageSectors &&
                       imageRead(uint64_t(lsn) * kSectorSize, bytes.data(), byteCount);
            }
            else if (buildVirtualIso() && uint64_t(lsn) + sectors <= volumeSectors)
            {
                read = true;
                for (uint32_t sector = 0u; sector < sectors; ++sector)
                {
                    if (!readVirtualSector(lsn + sector, bytes.data() + static_cast<size_t>(sector) * kSectorSize))
                    {
                        read = false;
                        break;
                    }
                }
            }
            return read;
        }

        bool readSectors(uint32_t lsn, uint32_t sectors, uint32_t destination)
        {
            // All current sector IO is synchronous. Track actual work, not host
            // path existence; deferred callback notification is separate.
            struct WorkGuard
            {
                CdvdDevice &device;
                CdvdDevice::Token token;
                ~WorkGuard() { device.finish(token); }
            } guard{device, device.begin(CdvdDevice::Work::Command)};
            if (sectors == 0u)
            {
                lastError = kCdvdErrorNone;
                return true;
            }
            const uint64_t byteCount64 = static_cast<uint64_t>(sectors) * kSectorSize;
            if (byteCount64 > IopMemory::RamSize || !memory.ownsRamRange(destination, static_cast<size_t>(byteCount64)))
            {
                lastError = kCdvdErrorRead;
                return false;
            }
            std::vector<uint8_t> bytes;
            if (!readSectorBytes(lsn, sectors, bytes) || !memory.writeRam(destination, bytes.data(), bytes.size()))
            {
                lastError = kCdvdErrorRead;
                return false;
            }
            lastError = kCdvdErrorNone;
            return true;
        }

        IopHost &host;
        IopMemory &memory;
        IopKernel &kernel;
        Callback callback;
        std::optional<CompletionCallback> completionCallback;
        CdvdDevice device;
        uint32_t currentLsn = 0u;
        uint32_t lastError = kCdvdErrorNone;
        uint32_t ncmdNextLsn = 0xFFFFFFFFu; // sector after the previous N-command read (sequential reads need no seek)
        uint32_t streamFlag = 0u;
        uint32_t lastReadTimeout = 0u;
        int interruptEventFlagId = 0;
        uint64_t imageHandle = 0u;
        std::string sourceImage, sourceRoot; // selected at first IO after reset, then stable
        bool sourceSelected = false;
        bool imageChecked = false, imageValid = false;
        uint64_t imageBytes = 0;
        uint32_t imageSectors = 0;
        ImageEntry imageRoot;
        bool virtualIsoBuilt = false;
        bool virtualIsoValid = false;
        mods::ModRoot modRoot;
        uint32_t volumeSectors = 0u;
        std::vector<IsoNode> nodes;
        std::unordered_map<uint32_t, std::array<uint8_t, kSectorSize>> metadataSectors;
    };

    IopCdvd::IopCdvd(IopHost &host, IopMemory &memory, IopKernel &kernel)
        : m_impl(std::make_unique<Impl>(host, memory, kernel))
    {
    }

    IopCdvd::~IopCdvd() = default;

    void IopCdvd::reset() noexcept
    {
        m_impl->reset();
    }

    CdvdDevice::Snapshot IopCdvd::deviceSnapshot() const
    {
        return m_impl->snapshot();
    }

    bool IopCdvd::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        return m_impl->dispatchImport(ordinal, cpu);
    }

    bool IopCdvd::hasControlServer(uint32_t sid) const
    {
        return (isCdvdfsvSid(sid) || sid == kCdvdfsvSearchSid || sid == kCdvdfsvNcmdSid) && m_impl->snapshot().prepared;
    }

    RpcResult IopCdvd::handleControlRpc(const RpcRequest &request)
    {
        return m_impl->handleControlRpc(request);
    }

    std::optional<IopCdvd::CompletionCallback> IopCdvd::takeCompletionCallback() noexcept
    {
        return m_impl->takeCompletionCallback();
    }
}
