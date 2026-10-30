#include <cstdlib>
#include "sif_packet_adapter.h"
#include <array>
#include <sstream>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        sif::Packet makePacket(uint32_t command, uint32_t size)
        {
            sif::Packet p{};
            p.size = size; p.words[0] = size; p.words[2] = command;
            return p;
        }
        struct GuestScratch
        {
            IopHost &host;
            uint32_t address;
            ~GuestScratch() { if (address) host.freeGuest(address); }
        };
    }

    void SifPacketAdapter::trace(const char *stage, uint32_t command, uint32_t detail)
    {
        // The wire trace is a debugging aid (one stderr line per SIF packet, ~25 % of the host time in a fast replay): PS2X_SIF_VERBOSE=1 turns it on.
        static const bool verbose = std::getenv("PS2X_SIF_VERBOSE") != nullptr;
        if (!verbose)
            return;
        std::ostringstream text;
        text << "[SIF:wire] " << stage << " cmd=0x" << std::hex << command
             << " detail=0x" << detail;
        host.log(LogLevel::Info, text.str());
    }

    bool SifPacketAdapter::guestRange(uint32_t address, uint32_t size)
    {
        if (!address || size > 32 * 1024 * 1024 || uint64_t(address) + size > 0x100000000ull)
            return false;
        std::array<uint8_t, 256> scratch{};
        while (size)
        {
            const uint32_t count = std::min<uint32_t>(size, scratch.size());
            if (!host.readGuest(address, scratch.data(), count)) return false;
            address += count; size -= count;
        }
        return true;
    }

    void SifPacketAdapter::reset()
    {
        connected = ready = rpcInitSeen = resetPending = false;
        eeBuffer = iopBuffer = 0;
        dataDestination = dataSize = 0;
        replies.clear(); servers.clear(); delayed.clear();
        host.resetGuestSifDelivery();
    }

    void SifPacketAdapter::reply(sif::Packet packet)
    {
        trace("IOP send queued", packet.command(), packet.words[8]);
        replies.push_back(packet);
    }

    void SifPacketAdapter::advance(uint64_t eeCycles)
    {
        clockEeCycles += eeCycles;
        while (!delayed.empty() && delayed.front().dueCycle <= clockEeCycles)
        {
            const DelayedCompletion item = delayed.front();
            delayed.pop_front();
            complete(item);
        }
    }

    void SifPacketAdapter::complete(const DelayedCompletion &item)
    {
        auto response = item.response;
        if (item.wireReply)
        {
            reply(response);
        }
        else
        {
            response.words[4] = response.words[6] = 0;
            const auto bytes = sif::encode(response);
            if (host.writeGuest(item.pollAddress, bytes.data(), bytes.size()))
                trace("EE polling completion", sif::Call, item.pollAddress);
        }
        if (item.followUpCommand)
        {
            // Asynchronous completion of the request, ordered after its END packet.
            auto command = makePacket(item.followUpCommand, 16);
            command.words[3] = item.followUpOption;
            reply(command);
        }
    }

    void SifPacketAdapter::pump()
    {
        if (connected && !ready && host.supportsGuestSifPackets())
        {
            // The adapter's BIND/CALL/RDATA handlers and backend are constructed
            // together by IopSubsystem. A legacy host with only sendSifCommand
            // does not have the guest packet return path and cannot announce RPC.
            ready = true;
            trace("protocol ready", sif::Init);
            auto response = makePacket(sif::SetSreg, 24);
            response.words[4] = 0; response.words[5] = 1;
            reply(response);
        }
        if (replies.empty()) return;
        const auto &packet = replies.front();
        const auto bytes = sif::encode(packet);
        if (host.deliverGuestSifPacket(eeBuffer, bytes.data(), packet.size))
        {
            trace("EE delivery accepted", packet.command(), eeBuffer);
            replies.pop_front();
        }
    }

    void SifPacketAdapter::receive(IopSubsystem &iop, const SifTransfer &transfer)
    {
        // INT_O denotes a SIF1 command DMA. Ordinary data DMA must never be
        // interpreted as a packet just because its bytes resemble a header.
        if (transfer.kind != SifTransferKind::SetDma ||
            transfer.phase != SifTransferPhase::AfterCopy) return;
        if (!(transfer.attributes & 0x40))
        {
            dataDestination = transfer.destinationAddress;
            dataSize = transfer.size;
            return;
        }
        if (transfer.size == sif::ResetPacket::WireSize)
        {
            receiveReset(iop, transfer);
            return;
        }
        if (transfer.size < 16 || transfer.size > 64)
        {
            trace("reject command size", 0, transfer.size);
            return;
        }
        std::array<uint8_t, 64> bytes{};
        sif::Packet packet{};
        if (!iop.readMemory(transfer.destinationAddress, bytes.data(), transfer.size) ||
            sif::parse(std::span(bytes).first(transfer.size), packet) != sif::ParseError::None)
        {
            trace("reject malformed", 0); return;
        }
        trace("IOP receive", packet.command(), packet.words[3]);
        if (connected && transfer.destinationAddress != iopBuffer)
        {
            trace("reject endpoint", packet.command(), transfer.destinationAddress); return;
        }
        if (packet.command() == sif::Init && packet.words[3] == 0)
        {
            if (!guestRange(packet.words[4], 64)) return;
            if (connected)
            {
                // Repeating INIT is not a reset and cannot discard in-flight RPC.
                if (eeBuffer != packet.words[4]) trace("reject reinit endpoint", sif::Init);
                return;
            }
            eeBuffer = packet.words[4]; iopBuffer = transfer.destinationAddress;
            connected = true;
        }
        else if (!ready)
            trace("reject before init", packet.command());
        else if (packet.command() == sif::Init)
        {
            rpcInitSeen = true;
            trace("RPC init event", packet.command());
        }
        else if (packet.command() == sif::ChangeAddress)
        {
            if (guestRange(packet.words[4], 64)) eeBuffer = packet.words[4];
        }
        else if (packet.command() == sif::Bind || packet.command() == sif::Call || packet.command() == sif::OtherData)
            rpc(iop, packet);
        else
            trace("reject unsolicited command", packet.command());
        pump();
    }

    bool SifPacketAdapter::takeResetRequest(sif::ResetPacket &request)
    {
        if (!resetPending) return false;
        request = resetRequest;
        resetPending = false;
        return true;
    }

    void SifPacketAdapter::receiveReset(IopSubsystem &iop, const SifTransfer &transfer)
    {
        std::array<uint8_t, sif::ResetPacket::WireSize> bytes{};
        sif::ResetPacket packet{};
        if (!iop.readMemory(transfer.destinationAddress, bytes.data(), bytes.size()) ||
            sif::parseReset(bytes, packet) != sif::ParseError::None)
        {
            trace("reject malformed", 0, transfer.size);
            return;
        }
        // The reset handler belongs to the SIFCMD endpoint, which exists once the EE has
        // sent INIT_CMD. The sender addresses the packet to the same IOP buffer.
        if (!connected || transfer.destinationAddress != iopBuffer)
        {
            trace("reject reset endpoint", sif::Reset, transfer.destinationAddress);
            return;
        }
        trace("IOP reset request", sif::Reset, packet.mode);
        resetRequest = packet;
        resetPending = true;
    }

    void SifPacketAdapter::rpc(IopSubsystem &iop, const sif::Packet &p)
    {
        if (!guestRange(p.words[5], 64) || !guestRange(p.words[7], p.command() == sif::OtherData ? 28 : 40))
        {
            trace("reject RPC addresses", p.command()); return;
        }
        auto response = makePacket(sif::End, 64);
        response.words[4] = p.words[4]; response.words[5] = p.words[5];
        response.words[6] = p.words[6]; response.words[7] = p.words[7];
        response.words[8] = p.command();
        if (p.command() == sif::Bind)
        {
            const uint32_t sid = p.words[8];
            if (iop.canBindRpc(sid))
            {
                uint32_t handle = 0;
                for (const auto &[key, server] : servers)
                    if (server.sid == sid) { handle = key; break; }
                if (!handle)
                {
                    // HLE endpoint storage, not a guest server-data layout.
                    handle = iop.allocateMemory(BufferCapacity + 64, 16);
                    if (handle) servers.emplace(handle, Server{sid, handle + 64});
                }
                if (!handle) { trace("resource exhausted", sif::Bind); return; }
                response.words[9] = handle;
                response.words[10] = servers.at(handle).buffer;
            }
            // The SDK returns a null server for an unavailable SID. It does not
            // invent a provider or a successful CALL result.
            trace("bind sid", sif::Bind, sid);
            trace("backend bind", sif::Bind, response.words[9]);
            reply(response); return;
        }
        if (p.command() == sif::OtherData)
        {
            const uint32_t size = p.words[10];
            if (!iop.isMemoryRange(p.words[8], size) || (size && !guestRange(p.words[9], size))) return;
            std::vector<uint8_t> data(size);
            if (size && (!iop.readMemory(p.words[8], data.data(), size) ||
                         !host.writeGuest(p.words[9], data.data(), size))) return;
            response.words[0] |= size << 8;
            response.words[1] = size ? p.words[9] : 0;
            response.words[9] = p.words[8]; response.words[10] = p.words[9]; response.words[11] = size;
            reply(response); return;
        }
        const auto found = servers.find(p.words[13]);
        if (found == servers.end()) { trace("reject server", sif::Call, p.words[13]); return; }
        const auto server = found->second;
        const uint32_t sendSize = p.words[9], receiveSize = p.words[11];
        const bool dataArrived = !sendSize || (dataDestination == server.buffer && dataSize >= sendSize);
        dataDestination = dataSize = 0;
        if (!iop.canBindRpc(server.sid) || sendSize > BufferCapacity || receiveSize > BufferCapacity ||
            !dataArrived ||
            p.extraSize() != sendSize || (sendSize && p.words[1] != server.buffer) ||
            (receiveSize && !guestRange(p.words[10], receiveSize)))
        { trace("reject CALL buffers", sif::Call); return; }
        GuestScratch send{host, sendSize ? host.allocateGuest(sendSize, 16) : 0};
        GuestScratch receive{host, receiveSize ? host.allocateGuest(receiveSize, 16) : 0};
        if ((sendSize && !send.address) || (receiveSize && !receive.address)) return;
        std::vector<uint8_t> data(std::max(sendSize, receiveSize));
        if (sendSize && (!iop.readMemory(server.buffer, data.data(), sendSize) ||
                        !host.writeGuest(send.address, data.data(), sendSize))) return;
        // Preserve bytes that the backend does not write, rather than inventing
        // zero-filled output for partially implemented HLE operations.
        if (receiveSize && (!host.readGuest(p.words[10], data.data(), receiveSize) ||
                           !host.writeGuest(receive.address, data.data(), receiveSize))) return;
        RpcRequest request{};
        request.packetTransport = true;
        request.clientAddress = p.words[7]; request.sid = server.sid;
        request.function = p.words[8]; request.send = {send.address, sendSize};
        request.receive = {receive.address, receiveSize};
        trace("backend CALL", sif::Call, request.function);
        const auto result = iop.handleRpc(request);
        if (!result.handled || !result.packetCompleted)
        { trace("backend not completed", sif::Call, request.function); return; }
        const uint32_t replySize = result.resultAddress ? receiveSize : 0;
        if (replySize && (result.resultAddress != receive.address ||
                         !host.readGuest(receive.address, data.data(), replySize) ||
                         !host.writeGuest(p.words[10], data.data(), replySize))) return;
        trace("backend complete", sif::Call, request.function);
        if (p.words[12])
        {
            response.words[0] |= replySize << 8;
            response.words[1] = replySize ? p.words[10] : 0;
        }
        DelayedCompletion item{clockEeCycles + result.completionDelayEeCycles, response, p.words[12] != 0u, p.words[5],
                               result.followUpCommand, result.followUpOption};
        if (result.completionDelayEeCycles == 0u)
        {
            complete(item);
        }
        else
        {
            // Other services keep answering while a drive read is in flight: only this call's END is held back.
            auto at = delayed.begin();
            while (at != delayed.end() && at->dueCycle <= item.dueCycle)
                ++at;
            delayed.insert(at, item);
        }
    }
}
