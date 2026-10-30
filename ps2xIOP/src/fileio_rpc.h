#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"
#include "ps2x/iop/sif_packet.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    // FILEIO RPC (SID 0x80000001) as spoken by the Sony SDK client of the game (the layout is taken
    // from the client code and from the FILEIO module in IOPRP300.IMG, not from PS2SDK's different
    // fileio.c protocol). The original module runs a worker per request. Each request is:
    //   +0 sema id  +4 EE mailbox pointer  +8 mailbox size (4)  then function specific fields,
    //   and a slot index at a function specific offset.
    // The worker performs the operation, writes the 32 bit result into the EE mailbox, fills slot
    // `index` (base + index * stride, base/stride announced by function 0xFF) with the header
    // {sema, function, 0, 0} and sends SIFCMD 0x80000011 (option = index). The EE handler signals
    // the sema, which wakes the caller. The RPC itself only answers "accepted" (1).
    //   0 open  flags@0xC name@0x14 index@0x414 -> fd | negative errno
    //   1 close fd@0xC index@0x10               -> 0
    //   2 read  fd@0xC dest@0x10 size@0x14 index@0x1C -> bytes read (data written in place)
    //   4 lseek fd@0xC offset@0x10 whence@0x14 index@0x18 -> new position
    // 255 init  {slot0, slot1}                  -> {0x30303033, 2} (value of the module's data)
    // Writes, directories, ioctl and the remaining functions are left uncompleted until observed.
    inline constexpr uint32_t kFileIoSid = 0x80000001u;

    class FileIoServer
    {
    public:
        explicit FileIoServer(IopHost &host) : m_host(host) {}
        ~FileIoServer() { reset(); }

        void reset()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto &[fd, file] : m_files)
                m_host.closeHostFile(file.handle);
            m_files.clear();
            m_nextFd = 3;
            m_slotBase = m_slotStride = 0;
        }

        [[nodiscard]] RpcResult handle(const RpcRequest &request)
        {
            RpcResult result{};
            result.handled = true;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (request.receive.address == 0u || request.receive.size < 4u)
                return unsupported(request, "no result buffer");
            std::vector<uint8_t> in(request.send.size);
            if (request.send.size && (request.send.address == 0u || !m_host.readGuest(request.send.address, in.data(), in.size())))
                return unsupported(request, "unreadable arguments");

            if (request.function == 0xFFu)
            {
                if (request.receive.size < 8u || (in.size() != 4u && in.size() != 8u))
                    return unsupported(request, "init arguments");
                m_slotBase = word(in, 0);
                m_slotStride = in.size() == 8u ? word(in, 4) - m_slotBase : 0u;
                const std::array<uint8_t, 8> init{0x33, 0x30, 0x30, 0x30, 2, 0, 0, 0};
                if (!m_host.writeGuest(request.receive.address, init.data(), init.size()))
                    return result;
                return finish(result, request);
            }

            // Function specific layout: index offset and minimum size.
            struct Layout { size_t index; size_t minimum; };
            Layout layout{};
            switch (request.function)
            {
            case 0: layout = {0x414, 0x418}; break;
            case 1: layout = {0x10, 0x14}; break;
            case 2: layout = {0x1C, 0x20}; break;
            case 4: layout = {0x18, 0x1C}; break;
            default: return unsupported(request, "function");
            }
            if (in.size() < layout.minimum || in.size() < 0x10u)
                return unsupported(request, "arguments");
            if (m_slotBase == 0u || m_slotStride == 0u)
                return unsupported(request, "no completion slots (function 0xFF not received)");

            const uint32_t sema = word(in, 0);
            const uint32_t mailbox = word(in, 4);
            const uint32_t index = word(in, layout.index);
            int32_t value = 0;
            switch (request.function)
            {
            case 0:
                if (!open(in, value)) return unsupported(request, "open arguments");
                break;
            case 1:
                value = close(word(in, 0xC));
                break;
            case 2:
                if (!read(in, value)) return unsupported(request, "read arguments");
                break;
            case 4:
                value = lseek(word(in, 0xC), static_cast<int32_t>(word(in, 0x10)), word(in, 0x14));
                break;
            }

            // 1. result into the caller's mailbox
            const std::array<uint8_t, 4> reply{uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
            if (mailbox == 0u || !m_host.writeGuest(mailbox, reply.data(), reply.size()))
                return unsupported(request, "mailbox write");
            // 2. completion slot: {sema, function, no inline copy}, empty read head/tail pieces
            std::array<uint8_t, 0x1C> slot{};
            auto put = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) slot[at + i] = uint8_t(v >> (8 * i)); };
            put(0, sema);
            put(4, request.function);
            if (!m_host.writeGuest(m_slotBase + index * m_slotStride, slot.data(), slot.size()))
                return unsupported(request, "slot write");
            // 3. accepted now, completion command afterwards
            const std::array<uint8_t, 4> accepted{1, 0, 0, 0};
            if (!m_host.writeGuest(request.receive.address, accepted.data(), accepted.size()))
                return result;
            result.followUpCommand = sif::FileIoComplete;
            result.followUpOption = index;
            return finish(result, request);
        }

    private:
        struct File
        {
            uint64_t handle = 0;
            uint64_t size = 0;
            uint64_t position = 0;
            std::string path;
        };

        static RpcResult finish(RpcResult result, const RpcRequest &request)
        {
            result.resultAddress = request.receive.address;
            result.packetCompleted = true;
            return result;
        }

        static uint32_t word(const std::vector<uint8_t> &data, size_t offset)
        {
            return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
                   (uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
        }

        static std::string cString(const std::vector<uint8_t> &data, size_t offset, size_t limit)
        {
            size_t length = 0;
            while (offset + length < data.size() && length < limit && data[offset + length] != 0u)
                ++length;
            return std::string(reinterpret_cast<const char *>(data.data() + offset), length);
        }

        RpcResult unsupported(const RpcRequest &request, const char *why)
        {
            RpcResult result{};
            result.handled = true;
            std::string words;
            if (request.send.address)
            {
                std::vector<uint8_t> raw(std::min<uint32_t>(request.send.size, 64u));
                if (m_host.readGuest(request.send.address, raw.data(), raw.size()))
                {
                    static const char hex[] = "0123456789abcdef";
                    for (uint8_t byte : raw) { words += hex[byte >> 4]; words += hex[byte & 15]; }
                }
            }
            m_host.log(LogLevel::Warning, "[FILEIO:HLE] unsupported call function=" + std::to_string(request.function) +
                                              " send=" + std::to_string(request.send.size) +
                                              " receive=" + std::to_string(request.receive.size) + " (" + why + ") head=" + words);
            return result;
        }

        bool open(const std::vector<uint8_t> &in, int32_t &value)
        {
            const uint32_t flags = word(in, 0xC);
            const std::string path = cString(in, 0x14, 0x400);
            constexpr uint32_t kWriteBits = 0x0002u | 0x0100u | 0x0200u | 0x0400u; // WRONLY APPEND CREAT TRUNC
            if (flags & kWriteBits)
            {
                m_host.log(LogLevel::Warning, "[FILEIO:HLE] write open refused: '" + path + "' flags=" + std::to_string(flags));
                return false;
            }
            const std::string hostPath = m_host.translateGuestPath(path);
            const uint64_t handle = hostPath.empty() ? 0u : m_host.openHostFile(hostPath);
            uint64_t size = 0;
            if (handle == 0u || !m_host.hostFileSize(handle, size))
            {
                if (handle) m_host.closeHostFile(handle);
                m_host.log(LogLevel::Info, "[FILEIO:HLE] open '" + path + "' -> ENOENT");
                value = -2;
                return true;
            }
            value = static_cast<int32_t>(m_nextFd++);
            m_files[value] = File{handle, size, 0, path};
            m_host.log(LogLevel::Info, "[FILEIO:HLE] open '" + path + "' -> fd " + std::to_string(value) + " size " + std::to_string(size));
            return true;
        }

        int32_t close(uint32_t fd)
        {
            const auto it = m_files.find(static_cast<int32_t>(fd));
            if (it == m_files.end()) return -9; // EBADF
            m_host.closeHostFile(it->second.handle);
            m_files.erase(it);
            return 0;
        }

        bool read(const std::vector<uint8_t> &in, int32_t &value)
        {
            const auto it = m_files.find(static_cast<int32_t>(word(in, 0xC)));
            if (it == m_files.end()) { value = -9; return true; }
            const uint32_t destination = word(in, 0x10);
            const uint32_t size = word(in, 0x14);
            File &file = it->second;
            const uint64_t available = file.position < file.size ? file.size - file.position : 0u;
            const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(size, available));
            std::vector<uint8_t> data(count);
            size_t bytesRead = 0;
            if (count && (!m_host.readHostFile(file.handle, file.position, data.data(), count, bytesRead) || bytesRead != count))
                return false;
            if (count && !m_host.writeGuest(destination, data.data(), count))
                return false;
            file.position += count;
            value = static_cast<int32_t>(count);
            return true;
        }

        int32_t lseek(uint32_t fdValue, int32_t offset, uint32_t whence)
        {
            const auto it = m_files.find(static_cast<int32_t>(fdValue));
            if (it == m_files.end()) return -9;
            File &file = it->second;
            int64_t base = 0;
            if (whence == 1) base = static_cast<int64_t>(file.position);
            else if (whence == 2) base = static_cast<int64_t>(file.size);
            else if (whence != 0) return -22; // EINVAL
            const int64_t target = base + offset;
            if (target < 0) return -22;
            file.position = static_cast<uint64_t>(target);
            return static_cast<int32_t>(file.position);
        }

        IopHost &m_host;
        std::mutex m_mutex;
        std::unordered_map<int32_t, File> m_files;
        int32_t m_nextFd = 3;
        uint32_t m_slotBase = 0;
        uint32_t m_slotStride = 0;
    };
}
