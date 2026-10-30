#pragma once

#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <array>
#include <cstdint>
#include <string>

namespace ps2x::iop::detail
{
    // IOP heap RPC (PS2SDK iopheap.c: SifAllocIopHeap/SifFreeIopHeap bind SID 0x80000003).
    // In the original system SYSMEM serves it; the IRX is not executed here, so the
    // same guest-visible contract is backed by the emulator's IOP allocator, the one
    // behind AllocSysMemory. Only functions with a documented contract are served:
    //   1: u32 size -> u32 IOP address (0 on failure)
    //   2: u32 IOP address -> u32 result (0 on success)
    // Anything else is left uncompleted: the game's other heap queries must be
    // identified from their consumers, not guessed.
    inline constexpr uint32_t kIopHeapSid = 0x80000003u;

    [[nodiscard]] inline bool isIopHeapSid(uint32_t sid) noexcept { return sid == kIopHeapSid; }

    [[nodiscard]] inline RpcResult handleIopHeapRpc(IopHost &host, IopMemory &memory, const RpcRequest &request)
    {
        RpcResult result{};
        if (!isIopHeapSid(request.sid))
            return result;
        result.handled = true;
        if ((request.function != 1u && request.function != 2u) || request.send.size != 4u ||
            request.receive.size != 4u || request.send.address == 0u || request.receive.address == 0u)
        {
            host.log(LogLevel::Warning, "[IOPHEAP:HLE] unsupported call function=" + std::to_string(request.function) +
                                            " send=" + std::to_string(request.send.size) +
                                            " receive=" + std::to_string(request.receive.size));
            return result;
        }
        std::array<uint8_t, 4> input{};
        if (!host.readGuest(request.send.address, input.data(), input.size()))
            return result;
        const uint32_t argument = uint32_t(input[0]) | (uint32_t(input[1]) << 8) |
                                  (uint32_t(input[2]) << 16) | (uint32_t(input[3]) << 24);
        uint32_t reply = 0u;
        if (request.function == 1u)
        {
            // sysmem blocks are 64 byte aligned.
            reply = memory.allocate(argument, 64u);
            host.log(LogLevel::Debug, "[IOPHEAP:HLE] alloc size=" + std::to_string(argument) +
                                          " -> " + std::to_string(reply));
        }
        else
        {
            reply = memory.freeAllocation(argument) ? 0u : 0xFFFFFFFFu;
            host.log(LogLevel::Debug, "[IOPHEAP:HLE] free addr=" + std::to_string(argument) +
                                          " -> " + std::to_string(int32_t(reply)));
        }
        const std::array<uint8_t, 4> output{
            uint8_t(reply), uint8_t(reply >> 8), uint8_t(reply >> 16), uint8_t(reply >> 24)};
        if (!host.writeGuest(request.receive.address, output.data(), output.size()))
            return result;
        result.resultAddress = request.receive.address;
        result.packetCompleted = true;
        return result;
    }
}
