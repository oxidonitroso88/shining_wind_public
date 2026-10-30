#include "iop_compat_test_support.h"
#include "ps2x/iop/sif_packet.h"
#include "emulator/imports/cdvdfsv_protocol.h"

using namespace ps2x::iop;
using namespace ps2x::iop::sif;
using iop_test::require;

struct CdvdHost : iop_test::Host
{
    bool failWrite = false;
    bool useImage = false;
    std::string hostPath(HostPathKind kind) const override
    { return useImage && kind == HostPathKind::CdImage ? std::string("test.iso") : std::string{}; }
    uint32_t scratch = 0x10000;
    std::vector<Packet> replies;
    bool writeGuest(uint32_t a, const void *p, size_t n) override
    { return !failWrite && Host::writeGuest(a, p, n); }
    bool supportsGuestSifPackets() const override { return true; }
    uint32_t allocateGuest(uint32_t n, uint32_t align) override
    {
        const auto a = (scratch + align - 1) & ~(align - 1);
        if (uint64_t(a) + n > guest.size()) return 0;
        scratch = a + n; return a;
    }
    bool deliverGuestSifPacket(uint32_t a, const void *p, size_t n) override
    {
        Packet packet{};
        require(parse({static_cast<const uint8_t *>(p), n}, packet) == ParseError::None, "invalid reply");
        require(writeGuest(a, p, n), "invalid receiver");
        replies.push_back(packet); return true;
    }
};

int main()
{
    try
    {
        size_t checks = 0;
        auto check = [&](bool b, const char *s) { require(b, s); ++checks; };
        CdvdHost host;
        // Protocol unit tests inject backend outcomes, never a fake successful
        // subsystem. Pending/cancelled/rejected cannot generate a response.
        auto probe = iop_test::request(0x80000592, 0);
        probe.send = {0x400, 4};
        const uint32_t zero = 0;
        require(host.writeGuest(0x400, &zero, 4), "probe input");
        for (auto phase : {CdvdDevice::Phase::Rejected, CdvdDevice::Phase::Pending,
                           CdvdDevice::Phase::Cancelled})
        {
            host.fill(0x800, 16, 0x5a);
            auto r = detail::handleCdvdfsv(host, probe, [&](detail::CdvdControl, uint32_t) {
                return CdvdDevice::Result{phase, {1, 2}, {}};
            });
            check(r.handled && !r.packetCompleted && host.word(0x800) == 0x5a5a5a5a,
                  "noncompleted backend fabricated response");
        }
        auto r = detail::handleCdvdfsv(host, probe, [](detail::CdvdControl, uint32_t) {
            return CdvdDevice::Result{CdvdDevice::Phase::Completed, {}, {}};
        });
        check(!r.packetCompleted, "completion without result accepted");
        r = detail::handleCdvdfsv(host, probe, [](detail::CdvdControl, uint32_t) {
            return CdvdDevice::Result{CdvdDevice::Phase::Completed, {}, 0};
        });
        check(r.packetCompleted && host.word(0x800) == 0, "operation failure converted to success");
        size_t backendCalls = 0;
        auto noEffects = [&](detail::CdvdControl, uint32_t) {
            ++backendCalls; return CdvdDevice::Result{};
        };
        probe.receive.address = 0xfffffffcu;
        (void)detail::handleCdvdfsv(host, probe, noEffects);
        probe.receive.address = 0x800; probe.send.size = 3;
        (void)detail::handleCdvdfsv(host, probe, noEffects);
        check(backendCalls == 0, "invalid buffers reached backend");
        host.fill(0x800, 32);
        IopSubsystem iop(host);
        check(iop.canBindRpc(0x80000592), "missing real CDVDFSV INIT provider");
        for (auto sid : {0x80000593u, 0x8000059au, 0x8000059cu})
            check(iop.canBindRpc(sid), "missing control provider");
        check(!iop.canBindRpc(0x80000596), "speculative service registered");
        auto request = iop_test::request(0x80000592, 0);
        request.send = {0x400, 4};
        auto arg = [&](uint32_t v) { require(host.writeGuest(0x400, &v, 4), "write arg"); };
        arg(0);
        auto result = iop.handleRpc(request);
        check(result.handled && result.packetCompleted && host.word(0x800) == 1, "INIT completion");
        check(iop.cdvdSnapshot().session == CdvdDevice::Session::Initialized, "INIT did not initialize backend");
        check(host.word(0x80c) == 0 && host.word(0x810) == 0xcccccccc, "INIT buffer layout/overrun");
        check(host.word(0x804) == detail::kCdvdfsvHleVersion && host.word(0x808) == detail::kCdvdmanVersion,
              "INIT provider metadata mismatch");
        check(iop.cdvdSnapshot().source == CdvdDevice::Source::Unconfigured, "INIT invented a mounted disc");
        request.sid = 0x80000593; request.function = 0x22; request.receive.size = 4;
        arg(2); result = iop.handleRpc(request);
        check(result.packetCompleted && host.word(0x800) == 1 && iop.cdvdSnapshot().selectedMode == 2, "Mmode shared state");
        request.sid = 0x8000059c; request.function = 0; arg(0);
        check(iop.handleRpc(request).packetCompleted && host.word(0x800) == 2, "DiskReady0");
        request.sid = 0x8000059a; arg(1);
        check(iop.handleRpc(request).packetCompleted && host.word(0x800) == 2, "DiskReady1");
        arg(8); host.fill(0x800, 16, 0x5a);
        check(!iop.handleRpc(request).packetCompleted && host.word(0x800) == 0x5a5a5a5a, "raw status fabricated");
        request.function = 99; arg(0);
        check(!iop.handleRpc(request).packetCompleted, "unknown function completed");
        request.sid = 0x80000592; request.function = 0; request.receive.size = 16;
        const auto valid = request;
        for (auto n : {0u, 3u, 5u})
        {
            request = valid; request.send.size = n;
            check(!iop.handleRpc(request).packetCompleted, "invalid send size completed");
        }
        for (auto n : {0u, 4u, 15u, 17u})
        {
            request = valid; request.receive.size = n;
            check(!iop.handleRpc(request).packetCompleted, "invalid response size completed");
        }
        for (auto a : {0u, 0xfffffffcu, 0x1ffffu})
        {
            request = valid; request.send.address = a;
            check(!iop.handleRpc(request).packetCompleted, "invalid request address completed");
            request = valid; request.receive.address = a;
            check(!iop.handleRpc(request).packetCompleted, "invalid response address completed");
        }
        request = valid; host.failWrite = true;
        check(!iop.handleRpc(request).packetCompleted, "failed write reported completion");
        host.failWrite = false;
        const auto epoch = iop.cdvdSnapshot().generation;
        iop.reset();
        check(iop.cdvdSnapshot().generation > epoch && iop.cdvdSnapshot().selectedMode == 0 &&
              iop.cdvdSnapshot().session == CdvdDevice::Session::Uninitialized, "reset leaked state");
        check(iop.handleRpc(valid).packetCompleted && iop.handleRpc(valid).packetCompleted, "repeated init failed");

        // Real SIF adapter transport, not direct calls standing in for wire completion.
        auto make = [](uint32_t command, uint32_t size) {
            Packet p{}; p.size = size; p.words[0] = size; p.words[2] = command; return p;
        };
        auto send = [&](const Packet &p) {
            auto bytes = encode(p);
            require(iop.writeMemory(0x400, bytes.data(), p.size), "packet write");
            iop.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x1000, 0x400, p.size, 0x40});
        };
        auto init = make(Init, 20); init.words[4] = 0x2000; send(init);
        check(host.replies.size() == 1 && host.replies.back().command() == SetSreg, "transport init");
        Packet oldCall{};
        uint32_t serial = 1;
        for (auto sid : {0x80000592u, 0x80000593u, 0x8000059cu, 0x8000059au})
        {
            auto bind = make(Bind, 64); bind.words[5] = 0x4000; bind.words[6] = serial++;
            bind.words[7] = 0x5000; bind.words[8] = sid; send(bind);
            const auto reply = host.replies.back();
            check(reply.words[8] == Bind && reply.words[9] != 0 && reply.words[6] == bind.words[6], "wire BIND");
            auto call = make(Call, 64); call.words[0] |= 4 << 8; call.words[1] = reply.words[10];
            call.words[5] = 0x4000; call.words[6] = serial++; call.words[7] = 0x5000;
            call.words[8] = sid == 0x80000593 ? 0x22 : 0; call.words[9] = 4;
            call.words[10] = 0x6000; call.words[11] = sid == 0x80000592 ? 16 : 4;
            call.words[12] = 1; call.words[13] = reply.words[9];
            const uint32_t mode = sid == 0x80000593 ? 2 : 0;
            require(iop.writeMemory(reply.words[10], &mode, 4), "payload write");
            iop.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x7000, reply.words[10], 4, 0});
            const auto count = host.replies.size(); send(call);
            check(host.replies.size() == count + 1 && host.replies.back().words[8] == Call &&
                  host.replies.back().words[6] == call.words[6], "wire CALL completion/correlation");
            check(host.word(0x6000) == (sid == 0x80000592 || sid == 0x80000593 ? 1u : 2u), "wire result");
            // A recognized SID with unsupported function must not deliver END.
            call.words[8] = 0xdeadbeef;
            require(iop.writeMemory(reply.words[10], &mode, 4), "negative payload write");
            iop.onSifTransfer({SifTransferKind::SetDma, SifTransferPhase::AfterCopy, 0x7000, reply.words[10], 4, 0});
            send(call);
            check(host.replies.size() == count + 1, "unsupported wire operation generated END");
            oldCall = call;
        }
        const auto count = host.replies.size();
        iop.reset(); send(init); send(oldCall);
        check(host.replies.size() == count + 1, "old handle survived reset");

        // sceCdSearchFile service (SID 0x80000597) over a small ISO9660 image.
        {
            constexpr uint32_t sector = 2048;
            std::vector<uint8_t> image(40u * sector, 0u);
            auto both32 = [&](size_t at, uint32_t v) {
                for (uint32_t i = 0; i < 4; ++i) { image[at + i] = uint8_t(v >> (8 * i)); image[at + 4 + i] = uint8_t(v >> ((3 - i) * 8)); }
            };
            auto record = [&](size_t base, size_t &cursor, const std::string &id, uint32_t lsn, uint32_t size, bool dir) {
                const size_t at = base + cursor, length = (33u + id.size() + 1u) & ~size_t(1);
                image[at] = uint8_t(length); both32(at + 2, lsn); both32(at + 10, size);
                image[at + 25] = dir ? 2 : 0; image[at + 32] = uint8_t(id.size());
                image[at + 28] = 1; image[at + 31] = 1; // volume sequence number 1 (LE16, BE16)
                std::memcpy(&image[at + 33], id.data(), id.size()); cursor += length;
            };
            const std::string dot(1, char(0)), dotdot(1, char(1));
            const size_t pvd = 16u * sector;
            image[pvd] = 1; std::memcpy(&image[pvd + 1], "CD001", 5); image[pvd + 6] = 1;
            both32(pvd + 80, 40);                                   // volume space size
            image[pvd + 120] = 1; image[pvd + 123] = 1;           // volume set size 1
            image[pvd + 124] = 1; image[pvd + 127] = 1;           // volume sequence 1
            image[pvd + 129] = 8; image[pvd + 130] = 8;           // logical block size 2048 (LE16, BE16)
            std::memcpy(&image[17u * sector + 1], "CD001", 5); image[17u * sector] = 255; image[17u * sector + 6] = 1;
            size_t rootAt = 156; record(pvd, rootAt, dot, 18, sector, true);
            size_t cursor = 0;
            record(18u * sector, cursor, dot, 18, sector, true);
            record(18u * sector, cursor, dotdot, 18, sector, true);
            record(18u * sector, cursor, "FILE.BIN;1", 30, 0x1234, false);
            CdvdHost sh; sh.useImage = true; sh.file = image;
            IopSubsystem search(sh);
            check(search.canBindRpc(0x80000597), "search-file service not bindable");
            auto writePath = [&](uint32_t at, const char *text) {
                std::vector<uint8_t> bytes(256, 0);
                for (size_t i = 0; text[i]; ++i) bytes[i] = uint8_t(text[i]);
                require(sh.writeGuest(at, bytes.data(), bytes.size()), "path write");
            };
            auto searchRequest = [&](uint32_t size) {
                auto r = iop_test::request(0x80000597, 0);
                r.send = {0x400, size}; r.receive = {0x900, 16};
                return r;
            };
            // 0x128 layout: fp(32) attrs(4) path(256) eedest(4)
            sh.fill(0x400, 0x130, 0);
            writePath(0x424, "FILE.BIN;1");
            const uint32_t attrs = 0xA5A5A5A5u, dest = 0x2000u;
            require(sh.writeGuest(0x420, &attrs, 4) && sh.writeGuest(0x524, &dest, 4), "search args");
            sh.fill(0x2000, 64, 0x5a); sh.fill(0x900, 16, 0x5a);
            auto found = search.handleRpc(searchRequest(0x128));
            check(found.handled && found.packetCompleted && sh.word(0x900) == 1, "search of an existing file");
            check(sh.word(0x2000) == 30 && sh.word(0x2004) == 0x1234 && std::memcmp(&sh.guest[0x2008], "FILE.BIN", 8) == 0,
                  "sceCdlFILE record (lsn/size/name) not delivered to the EE");
            check(sh.word(0x2020) == 0xA5A5A5A5u && sh.word(0x2024) == 0x5a5a5a5au, "attributes word / DMA length");
            writePath(0x424, "MISSING.BIN;1");
            sh.fill(0x2000, 64, 0x5a);
            auto missing = search.handleRpc(searchRequest(0x128));
            check(missing.packetCompleted && sh.word(0x900) == 0 && sh.word(0x2000) == 0, "missing file must answer 0 with a clean record");
            auto bad = searchRequest(0x120);
            sh.fill(0x900, 16, 0x5a);
            check(!search.handleRpc(bad).packetCompleted && sh.word(0x900) == 0x5a5a5a5au, "unknown search layout fabricated a reply");
            // N-command service: sceCdRead of the file's first sector straight into EE memory
            check(search.canBindRpc(0x80000595), "NCMD service not bindable");
            for (uint32_t i = 0; i < sector; ++i) image[30u * sector + i] = uint8_t(i * 7 + 3);
            CdvdHost rh; rh.useImage = true; rh.file = image;
            IopSubsystem reader(rh);
            auto readRequest = iop_test::request(0x80000595, 1);
            readRequest.send = {0x400, 24}; readRequest.receive = {0, 0};
            const uint32_t readArgs[6] = {30u, 1u, 0x3000u, 0u, 0x4000u, 0u};
            require(rh.writeGuest(0x400, readArgs, sizeof(readArgs)), "read args");
            rh.fill(0x3000, sector + 16, 0x5a); rh.fill(0x4000, 16, 0x5a);
            auto readDone = reader.handleRpc(readRequest);
            bool sectorOk = true;
            for (uint32_t i = 0; i < sector; ++i) sectorOk &= rh.guest[0x3000 + i] == uint8_t(i * 7 + 3);
            check(readDone.handled && readDone.packetCompleted && sectorOk, "NCMD read did not deliver the sector");
            check(rh.word(0x3000 + sector) == 0x5a5a5a5au, "NCMD read overran the requested sectors");
            check(rh.word(0x4000) == 0 && rh.word(0x4004) == 0, "unaligned head/tail block not cleared");
            // The drive delivers the data over time: the END packet is held back (seek for a jump, transfer time per sector),
            // and a read that continues where the previous one stopped needs no seek.
            const uint32_t nextArgs[6] = {31u, 1u, 0x3000u, 0u, 0x4000u, 0u};
            require(rh.writeGuest(0x400, nextArgs, sizeof(nextArgs)), "read args");
            auto sequentialRead = reader.handleRpc(readRequest);
            check(readDone.completionDelayEeCycles > 0u, "NCMD read completed instantly instead of after the drive latency");
            check(sequentialRead.packetCompleted && sequentialRead.completionDelayEeCycles > 0u &&
                      sequentialRead.completionDelayEeCycles < readDone.completionDelayEeCycles,
                  "a sequential NCMD read must take less time than one that needs a seek");
            const uint32_t pastEnd[6] = {5000u, 1u, 0x3000u, 0u, 0x4000u, 0u};
            require(rh.writeGuest(0x400, pastEnd, sizeof(pastEnd)), "read args");
            check(!reader.handleRpc(readRequest).packetCompleted, "read beyond the image completed");
            auto driveCmd = iop_test::request(0x80000595, 14);
            driveCmd.send = {0, 0}; driveCmd.receive = {0x900, 4};
            rh.fill(0x900, 4, 0x5a);
            auto ready = reader.handleRpc(driveCmd);
            check(ready.packetCompleted && rh.word(0x900) == 2, "NCMD disk ready with an image must answer 2");
            CdvdHost bare;
            IopSubsystem noMedia(bare);
            bare.fill(0x900, 4, 0x5a);
            check(noMedia.handleRpc(driveCmd).packetCompleted && bare.word(0x900) == 6, "NCMD disk ready without a source must answer 6");
            driveCmd.function = 6; driveCmd.receive = {0, 0};
            check(reader.handleRpc(driveCmd).packetCompleted, "NCMD standby not accepted");
            auto getError = iop_test::request(0x80000593, 4);
            getError.send = {0, 0}; getError.receive = {0x900, 4};
            rh.fill(0x900, 4, 0x5a);
            check(reader.handleRpc(getError).packetCompleted && rh.word(0x900) != 0x5a5a5a5au, "SCMD GetError must answer the last error code");
            auto otherFn = readRequest; otherFn.function = 4;
            check(!reader.handleRpc(otherFn).packetCompleted, "unobserved NCMD function completed");
            auto badFn = searchRequest(0x128); badFn.function = 3;
            check(!search.handleRpc(badFn).packetCompleted, "unknown search function completed");
        }
        std::cout << "PASS " << checks << " CDVDFSV checks\n";
        return 0;
    }
    catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
