#pragma once

#include "ps2x/iop/cdvd_device.h"
#include "ps2x/iop/iop_host.h"
#include <array>
#include <limits>

namespace ps2x::iop::detail
{
    enum class CdvdControl { Init, Mmode, DiskReady };
    // Version of this bounded HLE frontend, not an original IRX version.
    inline constexpr uint32_t kCdvdfsvHleVersion = 0x0100;
    // Existing CDVDMAN compatibility identity, also reported by sceCdSC(-9).
    inline constexpr uint32_t kCdvdmanVersion = 0x0226;

    // sceCdSearchFile service (PS2SDK libcdvd.c CD_SERVER_SEARCHFILE).
    inline constexpr uint32_t kCdvdfsvSearchSid = 0x80000597;
    // N-command service (PS2SDK ncmd.c CD_SERVER_NCMD): sector reads and drive commands.
    inline constexpr uint32_t kCdvdfsvNcmdSid = 0x80000595;

    inline bool isCdvdfsvSid(uint32_t sid)
    {
        return sid == 0x80000592 || sid == 0x80000593 ||
               sid == 0x8000059a || sid == 0x8000059c;
    }

    // Execute must not retain guest buffers. Production uses the serialized,
    // synchronous backend profile; a Pending result never becomes an RPC END.
    template <typename Execute>
    RpcResult handleCdvdfsv(IopHost &host, const RpcRequest &request, Execute execute)
    {
        RpcResult result{};
        if (!isCdvdfsvSid(request.sid)) return result;
        result.handled = true;
        const bool init = request.sid == 0x80000592;
        const bool mmode = request.sid == 0x80000593;
        const uint32_t responseSize = init ? 16 : 4;
        if (request.function != (mmode ? 0x22u : 0u) ||
            request.send.size != 4 || request.receive.size != responseSize) return result;
        auto valid = [&](GuestBuffer b) {
            uint32_t first = 0, last = 0;
            return b.address && b.size && b.size - 1 <= UINT32_MAX - b.address &&
                host.normalizeGuestAddress(b.address, first) &&
                host.normalizeGuestAddress(b.address + b.size - 1, last) &&
                uint64_t(first) + b.size - 1 == last;
        };
        std::array<uint8_t, 4> input{};
        std::array<uint8_t, 16> output{};
        if (!valid(request.send) || !valid(request.receive) ||
            !host.readGuest(request.send.address, input.data(), 4) ||
            !host.readGuest(request.receive.address, output.data(), responseSize)) return result;
        uint32_t argument = 0;
        for (unsigned i = 0; i < 4; ++i) argument |= uint32_t(input[i]) << (8 * i);
        // Only the audited initial control subset. Other SDK variants remain unsupported.
        if ((init && argument != 0) || (!init && !mmode && argument > 1)) return result;
        const auto completed = execute(init ? CdvdControl::Init :
                                      mmode ? CdvdControl::Mmode : CdvdControl::DiskReady, argument);
        if (completed.phase != CdvdDevice::Phase::Completed || !completed.value) return result;
        const std::array<uint32_t, 4> words{
            static_cast<uint32_t>(*completed.value), kCdvdfsvHleVersion, kCdvdmanVersion, 0};
        for (uint32_t i = 0; i < responseSize; ++i)
            output[i] = static_cast<uint8_t>(words[i / 4] >> (8 * (i % 4)));
        if (!host.writeGuest(request.receive.address, output.data(), responseSize)) return result;
        result.resultAddress = request.receive.address;
        result.packetCompleted = true;
        host.log(LogLevel::Debug, init ? "[CDVDFSV:HLE] Init completed" :
                 mmode ? "[CDVDFSV:HLE] Mmode completed" : "[CDVDFSV:HLE] DiskReady completed");
        return result;
    }
}
