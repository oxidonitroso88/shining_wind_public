#include "ps2x/iop/sif_packet.h"
#include "iop_compat_test_support.h"
#include <cstring>
#include <iostream>

using namespace ps2x::iop;
using namespace ps2x::iop::sif;
using iop_test::require;

struct WireHost final : iop_test::Host
{
    bool enabled = false;
    bool supported = true;
    bool supportsGuestSifPackets() const override { return supported; }
    uint32_t scratch = 0x10000;
    std::vector<Packet> delivered;
    std::vector<uint32_t> bootFlags;
    void notifyIopBoot(uint32_t smflagBits) override { bootFlags.push_back(smflagBits); }
    uint32_t allocateGuest(uint32_t size, uint32_t alignment) override
    {
        const auto result = (scratch + alignment - 1) & ~(alignment - 1);
        if (uint64_t(result) + size > guest.size()) return 0;
        scratch = result + size;
        return result;
    }
    bool deliverGuestSifPacket(uint32_t buffer, const void *data, size_t size) override
    {
        if (!enabled) return false;
        Packet p{};
        require(parse({static_cast<const uint8_t *>(data), size}, p) == ParseError::None, "bad outgoing packet");
        require(writeGuest(buffer, data, size), "bad EE buffer");
        delivered.push_back(p);
        return true;
    }
};

int main()
{
    try
    {
        size_t checks = 0;
        auto check = [&](bool value, const char *message) { require(value, message); ++checks; };
        auto make = [](uint32_t command, uint32_t size) {
            Packet p{}; p.size=size; p.words[0]=size; p.words[2]=command; return p;
        };
        auto test = [&](const Packet &p, ParseError expected) {
            auto bytes=encode(p); Packet out{};
            check(parse(std::span(bytes).first(p.size),out)==expected,"unexpected parser result");
            if(expected==ParseError::None)
                check(out.words==p.words,"wire round trip changed payload");
        };
        auto init=make(Init,20); init.words[4]=0x2000;
        test(init,ParseError::None);
        init.words[4]=0; test(init,ParseError::Payload);
        init.words[4]=0x2001; test(init,ParseError::Payload);
        auto sync=make(Init,16); sync.words[3]=1; test(sync,ParseError::None);
        sync.words[3]=2; test(sync,ParseError::Options);
        auto sreg=make(SetSreg,24); sreg.words[5]=1; test(sreg,ParseError::None);
        sreg.words[4]=32; test(sreg,ParseError::Payload);
        auto bind=make(Bind,64); bind.words[5]=0x4000; bind.words[7]=0x5000;
        bind.words[8]=0xfedcba98; test(bind,ParseError::None);
        auto call=make(Call,64); call.words[9]=32; call.words[11]=16;
        call.words[12]=1; test(call,ParseError::None);
        call.words[12]=2; test(call,ParseError::Payload);
        call.words[12]=0; call.words[9]=0xffffffff; test(call,ParseError::Payload);
        auto other=make(OtherData,64); other.words[10]=0x80000000;
        test(other,ParseError::Payload);
        test(make(0xdeadbeef,16),ParseError::UnknownCommand);
        auto wrong=make(Bind,64); wrong.words[0]=16; test(wrong,ParseError::Size);
        wrong=make(Bind,64); wrong.words[0]|=32<<8; test(wrong,ParseError::Payload);
        auto bytes=encode(bind); Packet out{}; out.words[15]=0x12345678;
        check(parse(std::span(bytes).first(15),out)==ParseError::Size,"short header accepted");
        check(out.words[15]==0x12345678,"failed parse modified output");

        // This is a backend contract probe, not an invented completion test:
        // DBCMAN version is an existing HLE operation with an observable reply.
        iop_test::Host host;
        IopSubsystem iop(host);
        check(iop.loadModule("rom0:DBCMAN").moduleId>0,"existing backend unavailable");
        auto request=iop_test::request(0x80001300,0x80001363);
        check(iop.handleRpc(request).handled,"version request not handled");
        check(host.word(request.receive.address)==0x310,"version reply mismatch");
        host.fill(request.receive.address,request.receive.size,0x5a);
        request.function=0xdeadbeef;
        const auto unknown=iop.handleRpc(request);
        check(host.word(request.receive.address)==0x5a5a5a5a,"unknown backend operation wrote reply");
        std::cout << "backend_unknown_handled=" << unknown.handled
                  << " signalCompletion=" << unknown.signalCompletion
                  << " signalNowaitCompletion=" << unknown.signalNowaitCompletion << '\n';
        check(!unknown.packetCompleted, "unknown CALL fabricated completion");

        WireHost wire;
        wire.supported=false;
        IopSubsystem endpoint(wire);
        auto sendPacket = [&](const Packet &p, uint32_t attributes = 0x40) {
            const auto bytes = encode(p);
            require(endpoint.writeMemory(0x400, bytes.data(), p.size), "IOP packet write failed");
            endpoint.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy,
                                   0x1000, 0x400, p.size, attributes});
        };
        sync.words[3]=1;
        sendPacket(sync);
        check(wire.delivered.empty(), "opt1 fabricated readiness before opt0");
        init.words[4]=0x2000;
        sendPacket(init,0);
        check(wire.delivered.empty(), "ordinary DMA interpreted as INIT");
        sendPacket(init);
        check(wire.delivered.empty(), "masked guest transport consumed response");
        wire.enabled=true;
        endpoint.runEeCycles(0);
        check(wire.delivered.empty(), "incomplete transport announced RPC readiness");
        wire.supported=true;
        // Backend is callable before readiness is delivered, not initialized by
        // writing the guest SREG. This is still an adapter test, not scheduler E2E.
        check(endpoint.loadModule("rom0:DBCMAN").moduleId>0, "backend preparation failed");
        wire.enabled=true;
        endpoint.runEeCycles(0);
        check(wire.delivered.size()==1, "SET_SREG not delivered");
        check(wire.delivered[0].command()==SetSreg && wire.delivered[0].words[4]==0 &&
              wire.delivered[0].words[5]==1, "SET_SREG payload changed");
        sendPacket(init); sendPacket(sync); sendPacket(sync);
        check(wire.delivered.size()==1, "duplicate init produced duplicate readiness");
        bind.words[8]=0x80001300; bind.words[4]=6; bind.words[6]=123;
        sendPacket(bind);
        check(wire.delivered.size()==2, "BIND response absent");
        const auto bound=wire.delivered.back();
        check(bound.command()==End && bound.words[8]==Bind && bound.words[9]!=0 &&
              bound.words[5]==bind.words[5] && bound.words[6]==123 && bound.words[7]==bind.words[7],
              "BIND correlation or backend handle lost");
        call=make(Call,64); call.words[4]=6; call.words[5]=0x4000; call.words[6]=124;
        call.words[7]=0x5000; call.words[8]=0x80001363; call.words[10]=0x6000;
        call.words[11]=16; call.words[12]=1; call.words[13]=bound.words[9];
        sendPacket(call);
        check(wire.word(0x6000)==0x310, "wire CALL failed to use existing backend");
        check(wire.delivered.size()==3 && wire.delivered.back().words[8]==Call &&
              wire.delivered.back().words[7]==0x5000, "CALL response lost client or command");
        call.words[12]=0; call.words[6]=125;
        sendPacket(call);
        check(wire.delivered.size()==3 && wire.word(0x4000+16)==0 && wire.word(0x4000+24)==0,
              "polling CALL fabricated END or failed completion DMA");
        call.words[12]=1; call.words[8]=0xdeadbeef;
        wire.fill(0x6000,16,0x5a);
        sendPacket(call);
        check(wire.delivered.size()==3 && wire.word(0x6000)==0x5a5a5a5a, "unsupported CALL fabricated response");
        call.words[8]=0x80001363; call.words[10]=0xfffffff0;
        sendPacket(call);
        check(wire.delivered.size()==3, "invalid receive address accepted");
        bind.words[8]=0x12345678;
        sendPacket(bind);
        check(wire.delivered.back().words[9]==0, "unavailable service fabricated a server");
        const auto count=wire.delivered.size();
        sendPacket(make(0xabcdef01,16));
        check(wire.delivered.size()==count, "unknown command answered");
        endpoint.reset();
        sendPacket(sync);
        check(wire.delivered.size()==count, "reset retained ready state");
        sendPacket(init);
        check(wire.delivered.size()==count+1, "reinitialization failed");
        call.words[10]=0x6000;
        sendPacket(call);
        check(wire.delivered.size()==count+1, "reset retained old server handle");
        auto rdata=make(OtherData,64);
        rdata.words[5]=0x4000; rdata.words[7]=0x5000;
        rdata.words[8]=0x1000; rdata.words[9]=0x6000; rdata.words[10]=4;
        const uint32_t contents=0x12345678;
        check(endpoint.writeMemory(0x1000,&contents,4),"prepare RDATA source");
        sendPacket(rdata);
        check(wire.word(0x6000)==contents && wire.delivered.back().words[8]==OtherData,
              "RDATA did not transfer IOP data and complete through END");
        const auto afterRdata=wire.delivered.size();
        rdata.words[8]=0x1fffff; sendPacket(rdata);
        check(wire.delivered.size()==afterRdata,"invalid IOP RDATA source answered");
        wire.enabled=false;
        sendPacket(bind); // pending null-server reply
        endpoint.reset(); wire.enabled=true; endpoint.runEeCycles(0);
        check(wire.delivered.size()==afterRdata,"queued packet survived reset");
        init.words[4]=0xfffffff0; sendPacket(init);
        check(wire.delivered.size()==afterRdata,"invalid INIT buffer announced readiness");

        // SifIopReset (SIFCMD 0x80000003): a 104 byte packet, outside the 64 byte RPC range.
        {
            auto makeReset = [](uint32_t size, uint32_t cid, uint32_t arglen, const char *arg) {
                std::vector<uint8_t> b(size, 0);
                auto put = [&](size_t at, uint32_t v) { for (int i = 0; i < 4 && at + i < b.size(); ++i) b[at + i] = uint8_t(v >> (8 * i)); };
                put(0, size & 255); put(8, cid); put(16, arglen); put(20, 0);
                for (size_t i = 0; arg[i] && 24 + i < b.size(); ++i) b[24 + i] = uint8_t(arg[i]);
                return b;
            };
            const char *image = "rom0:UDNL cdrom0:/MODULES/IOPRP300.IMG;1";
            const uint32_t imageLen = uint32_t(std::strlen(image));
            ResetPacket rp{};
            check(parseReset(makeReset(104, Reset, imageLen, image), rp) == ParseError::None, "valid reset rejected");
            check(rp.arglen == imageLen && rp.argument() == image, "reset argument lost");
            check(parseReset(makeReset(104, Reset, 79, image), rp) == ParseError::None, "arglen 79 rejected");
            check(parseReset(makeReset(104, Reset, 80, image), rp) == ParseError::Payload, "arglen 80 accepted");
            check(parseReset(makeReset(104, Bind, imageLen, image), rp) == ParseError::UnknownCommand, "wrong cid accepted");
            check(parseReset(makeReset(64, Reset, imageLen, image), rp) == ParseError::Size, "short reset accepted");
            check(parseReset(makeReset(108, Reset, imageLen, image), rp) == ParseError::Size, "long reset accepted");
            auto badHeader = makeReset(104, Reset, imageLen, image); badHeader[0] = 100;
            check(parseReset(badHeader, rp) == ParseError::Size, "header size mismatch accepted");
            auto garbageDsize = makeReset(104, Reset, imageLen, image); garbageDsize[1] = 0x55; garbageDsize[2] = 0xaa;
            check(parseReset(garbageDsize, rp) == ParseError::None, "uninitialised dsize must be ignored (SDK leaves it unset)");
            // The RPC parser keeps its 64 byte contract and does not learn reset.
            Packet rpcOut{};
            check(parse(makeReset(104, Reset, imageLen, image), rpcOut) == ParseError::Size, "RPC parser accepted 104 bytes");

            WireHost w2;
            IopSubsystem ep(w2);
            auto sendRaw = [&](const std::vector<uint8_t> &b, uint32_t attributes = 0x40) {
                require(ep.writeMemory(0x400, b.data(), b.size()), "IOP raw write failed");
                ep.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy,
                                  0x1000, 0x400, uint32_t(b.size()), attributes});
            };
            auto sendP = [&](const Packet &p) { const auto e = encode(p); sendRaw(std::vector<uint8_t>(e.begin(), e.begin() + p.size)); };
            const auto reset = makeReset(104, Reset, imageLen, image);
            w2.enabled = true;
            sendRaw(reset);
            ep.runEeCycles(RebootLatencyEeCycles * 2);
            check(w2.bootFlags.empty(), "reset before SIFCMD init was honoured");
            auto i0 = make(Init, 20); i0.words[4] = 0x2000;
            sendP(i0);
            check(w2.delivered.size() == 1 && w2.delivered[0].command() == SetSreg, "handshake before reset failed");
            check(ep.loadModule("rom0:DBCMAN").moduleId > 0, "backend preparation failed");
            auto b2 = make(Bind, 64); b2.words[4] = 6; b2.words[5] = 0x4000; b2.words[6] = 1; b2.words[7] = 0x5000; b2.words[8] = 0x80001300;
            sendP(b2);
            check(w2.delivered.size() == 2 && w2.delivered.back().words[9] != 0, "pre-reset bind failed");

            sendRaw(reset, 0);
            sendRaw(makeReset(105, Reset, imageLen, image));
            sendRaw(makeReset(200, Reset, imageLen, image));
            sendRaw(makeReset(104, Reset, 80, image));
            ep.runEeCycles(RebootLatencyEeCycles * 2);
            check(w2.bootFlags.empty(), "data DMA, bad size or bad arglen triggered a reboot");
            check(w2.delivered.size() == 2, "rejected reset produced traffic");

            sendRaw(reset);
            check(w2.bootFlags.empty(), "boot flags published synchronously with the reset packet");
            ep.runEeCycles(RebootLatencyEeCycles / 2);
            sendRaw(reset); // the rebooting IOP has no SIFCMD endpoint yet: ignored, the boot clock is not restarted
            ep.runEeCycles(RebootLatencyEeCycles - RebootLatencyEeCycles / 2 - 1);
            check(w2.bootFlags.empty(), "boot completed before its latency elapsed");
            ep.runEeCycles(1);
            check(w2.bootFlags.size() == 1 && w2.bootFlags[0] == 0x70000, "boot flags (SIFINIT|CMDINIT|BOOTEND) not published once");
            ep.runEeCycles(RebootLatencyEeCycles * 4);
            check(w2.bootFlags.size() == 1, "boot flags published twice");

            sendP(b2);
            check(w2.delivered.size() == 2, "old SIFCMD endpoint survived reboot");
            auto sr = w2.delivered.size();
            sendP(i0);
            check(w2.delivered.size() == sr + 1 && w2.delivered.back().command() == SetSreg, "SIFCMD did not re-handshake after reboot");
            sendP(b2);
            check(w2.delivered.size() == sr + 2 && w2.delivered.back().words[9] == 0, "service module survived the IOP reboot");
            ep.reset();
            ep.runEeCycles(RebootLatencyEeCycles * 2);
            check(w2.bootFlags.size() == 1, "explicit reset left a pending boot");
        }

        // IOP heap RPC (SID 0x80000003): SifAllocIopHeap / SifFreeIopHeap over the packet transport.
        {
            WireHost hw;
            IopSubsystem hp(hw);
            hw.enabled = true;
            auto put = [&](const Packet &p) {
                const auto e = encode(p);
                require(hp.writeMemory(0x400, e.data(), p.size), "IOP heap packet write failed");
                hp.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x1000, 0x400, p.size, 0x40});
            };
            auto i0 = make(Init, 20); i0.words[4] = 0x2000; put(i0);
            auto hb = make(Bind, 64); hb.words[4] = 6; hb.words[5] = 0x4000; hb.words[6] = 1; hb.words[7] = 0x5000; hb.words[8] = 0x80000003;
            put(hb);
            const auto bound = hw.delivered.back();
            check(bound.command() == End && bound.words[9] != 0, "IOP heap SID not bindable");
            auto heapCall = [&](uint32_t function, uint32_t value, uint32_t sendSize = 4, uint32_t receiveSize = 4) {
                // The SDK sends the argument through the server buffer by data DMA first.
                const uint32_t arg = value;
                require(hp.writeMemory(bound.words[10], &arg, 4), "heap arg write failed");
                hp.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x1000, bound.words[10], 4, 0});
                Packet c = make(Call, 64); c.words[0] |= sendSize << 8; c.words[1] = bound.words[10];
                c.words[4] = 6; c.words[5] = 0x4000; c.words[6] = 2; c.words[7] = 0x5000; c.words[8] = function;
                c.words[9] = sendSize; c.words[10] = 0x6000; c.words[11] = receiveSize; c.words[12] = 1; c.words[13] = bound.words[9];
                hw.fill(0x6000, 16, 0x5a);
                const auto before = hw.delivered.size();
                put(c);
                return hw.delivered.size() == before + 1;
            };
            check(heapCall(1, 0x1000), "heap alloc not completed");
            const uint32_t block = hw.word(0x6000);
            check(block != 0 && block % 64 == 0 && hp.isMemoryRange(block, 0x1000), "heap alloc returned an unusable IOP block");
            check(heapCall(1, 0x1000) && hw.word(0x6000) != block, "second alloc reused a live block");
            check(heapCall(2, block) && hw.word(0x6000) == 0, "heap free failed");
            check(heapCall(2, block) && hw.word(0x6000) == 0xffffffffu, "double free reported success");
            check(heapCall(1, 0x7fffffffu) && hw.word(0x6000) == 0, "impossible alloc did not report NULL");
            const auto n = hw.delivered.size();
            check(!heapCall(5, 0) && hw.word(0x6000) == 0x5a5a5a5au, "undocumented heap function fabricated a reply");
            check(!heapCall(1, 0x100, 4, 8) && hw.delivered.size() == n, "wrong receive size completed");
        }

        // LOADFILE RPC (SID 0x80000006): SifLoadModule through the packet transport.
        {
            WireHost lw;
            IopSubsystem lp(lw);
            lw.enabled = true;
            auto put = [&](const Packet &p) {
                const auto e = encode(p);
                require(lp.writeMemory(0x400, e.data(), p.size), "LOADFILE packet write failed");
                lp.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x1000, 0x400, p.size, 0x40});
            };
            auto i0 = make(Init, 20); i0.words[4] = 0x2000; put(i0);
            auto bindSid = [&](uint32_t sid) {
                auto hb = make(Bind, 64); hb.words[4] = 6; hb.words[5] = 0x4000; hb.words[6] = 1; hb.words[7] = 0x5000; hb.words[8] = sid;
                put(hb);
                return lw.delivered.back();
            };
            const auto lf = bindSid(0x80000006);
            check(lf.command() == End && lf.words[9] != 0, "LOADFILE SID not bindable");
            check(bindSid(0x80001300).words[9] == 0, "DBCMAN bindable before its module was loaded");
            auto loadCall = [&](uint32_t function, const char *path, uint32_t sendSize = 512, uint32_t receiveSize = 8) {
                std::vector<uint8_t> arg(512, 0);
                for (size_t i = 0; path[i]; ++i) arg[8 + i] = uint8_t(path[i]);
                require(lp.writeMemory(lf.words[10], arg.data(), arg.size()), "LOADFILE arg write failed");
                lp.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x1000, lf.words[10], 512, 0});
                Packet c = make(Call, 64); c.words[0] |= sendSize << 8; c.words[1] = sendSize ? lf.words[10] : 0;
                c.words[4] = 6; c.words[5] = 0x4000; c.words[6] = 2; c.words[7] = 0x5000; c.words[8] = function;
                c.words[9] = sendSize; c.words[10] = 0x6000; c.words[11] = receiveSize; c.words[12] = 1; c.words[13] = lf.words[9];
                lw.fill(0x6000, 16, 0x5a);
                const auto before = lw.delivered.size();
                put(c);
                return lw.delivered.size() == before + 1;
            };
            check(loadCall(0, "rom0:DBCMAN"), "LOADFILE module load not completed");
            check(int32_t(lw.word(0x6000)) > 0, "LOADFILE reported no module id for a loadable module");
            check(bindSid(0x80001300).words[9] != 0, "loaded module did not become a bindable service");
            check(loadCall(0, "cdrom0:/NO_SUCH_MODULE.IRX;1") && lw.word(0x6000) == 0xffffffffu &&
                  lw.word(0x6004) == 0xffffffffu, "missing module did not report failure");
            check(loadCall(6, "") && lw.word(0x6000) == 0xffffffffu, "LOADFILE module-buffer load of an invalid image did not report failure");
            check(!loadCall(7, "rom0:DBCMAN") && lw.word(0x6000) == 0x5a5a5a5au, "unimplemented LOADFILE function fabricated a reply");
            check(loadCall(0xff, "", 0, 4) && lw.word(0x6000) == 0x30303033u, "LOADFILE version (\"3000\") not reported");
            check(!loadCall(0, "rom0:DBCMAN", 256), "short LOADFILE argument completed");
        }

        // FILEIO RPC (SID 0x80000001), Sony client protocol: accepted reply, EE mailbox + completion
        // slot writes, and an asynchronous SIFCMD 0x80000011 after the END packet.
        {
            WireHost fw;
            IopSubsystem fp(fw);
            fw.enabled = true;
            for (int i = 0; i < 100; ++i) fw.file.push_back(uint8_t(i + 1));
            auto put = [&](const Packet &p) {
                const auto e = encode(p);
                require(fp.writeMemory(0x400, e.data(), p.size), "FILEIO packet write failed");
                fp.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x1000, 0x400, p.size, 0x40});
            };
            auto i0 = make(Init, 20); i0.words[4] = 0x2000; put(i0);
            auto hb = make(Bind, 64); hb.words[4] = 6; hb.words[5] = 0x4000; hb.words[6] = 1; hb.words[7] = 0x5000; hb.words[8] = 0x80000001;
            put(hb);
            const auto fio = fw.delivered.back();
            check(fio.command() == End && fio.words[9] != 0, "FILEIO SID not bindable");
            auto w32 = [](std::vector<uint8_t> &v, size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (8 * i)); };
            constexpr uint32_t kBase = 0x8000, kStride = 0x440, kMailbox = 0x7300;
            // Returns the number of packets delivered by the call.
            auto fioCall = [&](uint32_t function, const std::vector<uint8_t> &arg, uint32_t receiveSize = 4) {
                require(fp.writeMemory(fio.words[10], arg.data(), arg.size()), "FILEIO arg write failed");
                fp.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x1000, fio.words[10], uint32_t(arg.size()), 0});
                Packet c = make(Call, 64); c.words[0] |= uint32_t(arg.size()) << 8; c.words[1] = arg.empty() ? 0 : fio.words[10];
                c.words[4] = 6; c.words[5] = 0x4000; c.words[6] = 2; c.words[7] = 0x5000; c.words[8] = function;
                c.words[9] = uint32_t(arg.size()); c.words[10] = 0x6000; c.words[11] = receiveSize; c.words[12] = 1; c.words[13] = fio.words[9];
                fw.fill(0x6000, 16, 0x5a); fw.fill(kMailbox, 4, 0x77);
                const auto before = fw.delivered.size();
                put(c);
                fp.runEeCycles(1); // the transport delivers one queued packet per pump
                return fw.delivered.size() - before;
            };
            auto request = [&](size_t size, uint32_t sema, size_t indexAt, uint32_t index) {
                std::vector<uint8_t> a(size, 0); w32(a, 0, sema); w32(a, 4, kMailbox); w32(a, 8, 4); w32(a, indexAt, index);
                return a;
            };
            // function 0xFF: slot geometry + version reply
            std::vector<uint8_t> init8(8, 0); w32(init8, 0, kBase); w32(init8, 4, kBase + kStride);
            check(fioCall(0xff, init8, 8) == 1 && fw.word(0x6000) == 0x30303033u && fw.word(0x6004) == 2u, "FILEIO init/version reply");
            // open
            auto openReq = request(0x418, 7, 0x414, 5); w32(openReq, 0xC, 1);
            const char *name = "cdrom0:TEST.BIN;1";
            for (size_t i = 0; name[i]; ++i) openReq[0x14 + i] = uint8_t(name[i]);
            check(fioCall(0, openReq) == 2, "open must deliver END then the completion command");
            check(fw.delivered[fw.delivered.size() - 2].command() == End && fw.delivered.back().command() == FileIoComplete &&
                  fw.delivered.back().words[3] == 5, "completion command order or slot index");
            check(fw.word(0x6000) == 1, "RPC reply is not 'accepted'");
            check(int32_t(fw.word(kMailbox)) == 3, "open result (fd) not written to the EE mailbox");
            check(fw.word(kBase + 5 * kStride) == 7 && fw.word(kBase + 5 * kStride + 4) == 0 &&
                  fw.word(kBase + 5 * kStride + 0x14) == 0 && fw.word(kBase + 5 * kStride + 0x18) == 0, "completion slot header");
            auto writeOpen = openReq; w32(writeOpen, 0xC, 0x0602);
            check(fioCall(0, writeOpen) == 0 && fw.word(kMailbox) == 0x77777777u, "write open fabricated a result");
            // read
            auto readReq = request(0x20, 8, 0x1C, 2); w32(readReq, 0xC, 3); w32(readReq, 0x10, 0x7000); w32(readReq, 0x14, 40);
            check(fioCall(2, readReq) == 2 && fw.word(kMailbox) == 40, "FILEIO read count");
            bool exact = true;
            for (int i = 0; i < 40; ++i) exact &= fw.guest[0x7000 + i] == uint8_t(i + 1);
            check(exact, "FILEIO read bytes differ from the file");
            check(fw.word(kBase + 2 * kStride) == 8 && fw.word(kBase + 2 * kStride + 4) == 2, "read completion slot");
            // lseek
            auto seekReq = request(0x1C, 9, 0x18, 1); w32(seekReq, 0xC, 3); w32(seekReq, 0x10, uint32_t(-10)); w32(seekReq, 0x14, 2);
            check(fioCall(4, seekReq) == 2 && fw.word(kMailbox) == 90, "FILEIO lseek(END,-10)");
            w32(readReq, 0x14, 50);
            check(fioCall(2, readReq) == 2 && fw.word(kMailbox) == 10 && fw.guest[0x7000] == 91, "FILEIO read is clamped at EOF");
            check(fioCall(2, readReq) == 2 && fw.word(kMailbox) == 0, "FILEIO read at EOF");
            // close
            auto closeReq = request(0x14, 10, 0x10, 3); w32(closeReq, 0xC, 3);
            check(fioCall(1, closeReq) == 2 && fw.word(kMailbox) == 0, "FILEIO close");
            check(fioCall(2, readReq) == 2 && int32_t(fw.word(kMailbox)) == -9, "read after close did not report EBADF");
            // unsupported function: no END, no command, no fabricated reply
            check(fioCall(3, request(0x30, 11, 0x2C, 4)) == 0 && fw.word(0x6000) == 0x5a5a5a5au, "FILEIO write fabricated a reply");
            fw.file.clear();
            auto missing = openReq; const char *miss = "cdrom0:MISSING.BIN;1";
            std::fill(missing.begin() + 0x14, missing.begin() + 0x40, uint8_t(0));
            for (size_t i = 0; miss[i]; ++i) missing[0x14 + i] = uint8_t(miss[i]);
            check(fioCall(0, missing) == 2 && int32_t(fw.word(kMailbox)) == -2, "missing file did not report ENOENT");
        }
        std::cout << "PASS " << checks << " parser/backend contract checks\n";
        return 0;
    }
    catch(const std::exception &error)
    {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
