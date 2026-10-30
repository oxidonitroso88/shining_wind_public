#pragma once

#include "ps2x/iop/iop_subsystem.h"
#include "ps2x/iop/sif_packet.h"
#include <deque>
#include <unordered_map>

namespace ps2x::iop::detail
{
    // Serial SIF wire endpoint. The backend API continues to use EE buffers;
    // input DMA is staged explicitly from the distinct IOP address space.
    class SifPacketAdapter
    {
    public:
        explicit SifPacketAdapter(IopHost &host) : host(host) {}
        void reset();
        void receive(IopSubsystem &iop, const SifTransfer &transfer);
        void pump();
        // EE cycles that elapsed; releases the END packets of RPC calls whose device latency has passed.
        void advance(uint64_t eeCycles);
        // A validated SIFCMD reset (SifIopReset) waits here until the owning subsystem
        // reboots the IOP; the adapter cannot reset itself from inside receive().
        bool takeResetRequest(sif::ResetPacket &request);

    private:
        struct Server { uint32_t sid, buffer; };
        static constexpr uint32_t BufferCapacity = 4 * 1024;
        bool guestRange(uint32_t address, uint32_t size);
        void rpc(IopSubsystem &iop, const sif::Packet &packet);
        void receiveReset(IopSubsystem &iop, const SifTransfer &transfer);
        void trace(const char *stage, uint32_t command, uint32_t detail = 0);
        void reply(sif::Packet packet);
        IopHost &host;
        bool connected = false;
        bool ready = false;
        bool rpcInitSeen = false;
        bool resetPending = false;
        sif::ResetPacket resetRequest{};
        uint32_t eeBuffer = 0;
        uint32_t iopBuffer = 0;
        uint32_t dataDestination = 0, dataSize = 0;
        std::deque<sif::Packet> replies;
        struct DelayedCompletion
        {
            uint64_t dueCycle;
            sif::Packet response;
            bool wireReply;
            uint32_t pollAddress;
            uint32_t followUpCommand, followUpOption;
        };
        std::deque<DelayedCompletion> delayed;
        uint64_t clockEeCycles = 0;
        void complete(const DelayedCompletion &item);
        std::unordered_map<uint32_t, Server> servers;
    };
}
