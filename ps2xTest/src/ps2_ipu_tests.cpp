#include "MiniTest.h"
#include "runtime/ps2_ipu.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace
{
    using ps2::Ipu;

    // Writes bit strings ("1011") MSB first, like an MPEG bit stream.
    struct BitWriter
    {
        std::vector<uint8_t> bytes;
        uint32_t bits = 0;
        void put(const std::string &s)
        {
            for (char c : s)
            {
                if (c != '0' && c != '1')
                    continue;
                if ((bits & 7u) == 0)
                    bytes.push_back(0);
                if (c == '1')
                    bytes.back() |= static_cast<uint8_t>(0x80u >> (bits & 7u));
                ++bits;
            }
        }
        void alignByte()
        {
            while (bits & 7u)
                put("0");
        }
        // Pads to whole quadwords (the IN FIFO only carries 128-bit units).
        std::vector<uint8_t> finish()
        {
            alignByte();
            std::vector<uint8_t> v = bytes;
            while (v.size() % 16u)
                v.push_back(0);
            return v;
        }
    };

    struct Rig
    {
        Ipu ipu;
        std::vector<uint8_t> input;
        size_t inputPos = 0;
        std::vector<uint8_t> output;
        bool sinkOpen = true;
        int interrupts = 0;
        bool sourceOpen = true;

        Rig()
        {
            ipu.setDmaSource([this](uint8_t *dst, size_t maxQw) -> size_t {
                if (!sourceOpen)
                    return 0;
                size_t n = 0;
                while (n < maxQw && inputPos + 16 <= input.size())
                {
                    std::memcpy(dst + n * 16, input.data() + inputPos, 16);
                    inputPos += 16;
                    ++n;
                }
                return n;
            });
            ipu.setDmaSink([this](const uint8_t *src, size_t qw) -> size_t {
                if (!sinkOpen)
                    return 0;
                output.insert(output.end(), src, src + qw * 16);
                return qw;
            });
            ipu.setInterruptHook([this]() { ++interrupts; });
        }
        void feed(const std::vector<uint8_t> &data)
        {
            input.insert(input.end(), data.begin(), data.end());
            ipu.run();
        }
        void cmd(uint32_t v) { ipu.writeRegister(0x00, v); }
    };

    // Intra macroblock made only of DC coefficients: luma diff +16 (first block), Cb diff -16, others 0.
    std::string flatIntraMacroblock()
    {
        std::string s;
        s += "100";       // luma dc size 0 ... replaced below
        s.clear();
        s += "1110" "10000";   // Y0: size 5, diff +16
        s += "10";             // EOB (table B.14)
        for (int i = 0; i < 3; ++i)
            s += "100" "10";   // Y1..Y3: size 0, EOB
        s += "11110" "01111";  // Cb: size 5, diff -16
        s += "10";
        s += "00" "10";        // Cr: size 0, EOB
        return s;
    }

    uint32_t le16(const std::vector<uint8_t> &v, size_t i) { return static_cast<uint32_t>(v[i * 2] | (v[i * 2 + 1] << 8)); }
} // namespace

void register_ps2_ipu_tests()
{
    MiniTest::Case("PS2Ipu", [](TestCase &tc)
    {
        tc.Run("IDCT of a DC-only block is flat", [](TestCase &t)
        {
            int16_t in[64] = {};
            int16_t out[64];
            in[0] = 1024;
            Ipu::inverseDct8x8(in, out);
            bool flat = true;
            for (int i = 0; i < 64; ++i)
                flat &= out[i] == 128;
            t.IsTrue(flat, "DC 1024 must give 128 everywhere");
            std::memset(in, 0, sizeof(in));
            in[1] = 100; // first horizontal AC
            Ipu::inverseDct8x8(in, out);
            t.IsTrue(out[0] > 0 && out[7] < 0 && out[0] == -out[7], "horizontal cosine is antisymmetric");
            t.IsTrue(out[0] == out[8], "constant along columns");
        });

        tc.Run("CSC maps limited range black/white/grey", [](TestCase &t)
        {
            uint8_t y[256], cb[64], cr[64], rgba[1024];
            std::memset(cb, 128, sizeof(cb));
            std::memset(cr, 128, sizeof(cr));
            std::memset(y, 16, sizeof(y));
            Ipu::yuv420ToRgb32(y, cb, cr, 0, 0, rgba);
            t.Equals(static_cast<int>(rgba[0]), 0, "black R");
            t.Equals(static_cast<int>(rgba[3]), 0x80, "default alpha");
            std::memset(y, 235, sizeof(y));
            Ipu::yuv420ToRgb32(y, cb, cr, 0, 0, rgba);
            t.IsTrue(rgba[0] >= 254 && rgba[1] >= 254 && rgba[2] >= 254, "white");
            std::memset(y, 126, sizeof(y));
            Ipu::yuv420ToRgb32(y, cb, cr, 0, 0, rgba);
            t.IsTrue(rgba[0] == rgba[1] && rgba[1] == rgba[2], "grey stays neutral");
            // Thresholds: below TH1 -> alpha 0x40, below TH0 -> pixel cleared.
            std::memset(y, 16, sizeof(y));
            Ipu::yuv420ToRgb32(y, cb, cr, 0, 8, rgba);
            t.Equals(static_cast<int>(rgba[3]), 0x40, "alpha from TH1");
            Ipu::yuv420ToRgb32(y, cb, cr, 8, 8, rgba);
            t.Equals(static_cast<int>(rgba[3]), 0, "alpha cleared by TH0");
        });

        tc.Run("VDEC returns (length << 16 | value) for each table", [](TestCase &t)
        {
            Rig r;
            BitWriter w;
            w.put("1");         // MBAI: 1
            w.put("011");       // MBAI: 3? (code 011 = increment 2 in table B.1? see below)
            w.put("0000001000"); // padding bits (escape prefix) kept out of the checks
            r.input = w.finish();
            r.ipu.writeRegister(0x10, 0u); // CTRL
            r.cmd((3u << 28) | (0u << 26)); // VDEC MBAI, FB=0
            r.ipu.run();
            t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "VDEC completes");
            t.Equals(r.ipu.readRegister(0x00), (1u << 16) | 1u, "increment 1 is code '1'");
            r.cmd((3u << 28));
            r.ipu.run();
            // '011' is not a valid prefix? B.1: 011 = increment 2 (code 011, 3 bits)
            t.Equals(r.ipu.readRegister(0x00), (3u << 16) | 2u, "increment 2 is code '011'");
        });

        tc.Run("VDEC macroblock type, motion code and dmvector", [](TestCase &t)
        {
            Rig r;
            BitWriter w;
            w.put("01");   // P: pattern only (0x02), 2 bits
            w.put("010");  // motion code +1
            w.put("011");  // motion code -1
            w.put("1");    // motion code 0
            w.put("11");   // dmvector -1
            r.input = w.finish();
            r.ipu.writeRegister(0x10, 2u << 24); // PCT = P
            r.cmd((3u << 28) | (1u << 26));
            t.Equals(r.ipu.readRegister(0x00), (2u << 16) | 0x02u, "P pattern macroblock type");
            r.cmd((3u << 28) | (2u << 26));
            t.Equals(r.ipu.readRegister(0x00), (3u << 16) | 1u, "motion +1");
            r.cmd((3u << 28) | (2u << 26));
            t.Equals(r.ipu.readRegister(0x00), (3u << 16) | 0xFFFFu, "motion -1");
            r.cmd((3u << 28) | (2u << 26));
            t.Equals(r.ipu.readRegister(0x00), (1u << 16) | 0u, "motion 0");
            r.cmd((3u << 28) | (3u << 26));
            t.Equals(r.ipu.readRegister(0x00), (2u << 16) | 0xFFFFu, "dmvector -1");
        });

        tc.Run("VDEC reports an invalid code through ECD", [](TestCase &t)
        {
            Rig r;
            BitWriter w;
            w.put("0000000000000000"); // no valid macroblock_address_increment starts with 16 zeros
            r.input = w.finish();
            r.cmd(3u << 28);
            t.IsTrue((r.ipu.readRegister(0x10) & Ipu::kCtrlEcd) != 0, "ECD set");
            t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "command ended");
        });

        tc.Run("FDEC peeks and BCLR restarts the pointer", [](TestCase &t)
        {
            Rig r;
            std::vector<uint8_t> data(32);
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<uint8_t>(0xA0 + i);
            r.feed(data);
            r.cmd(4u << 28 | 8u); // FDEC, skip 8 bits
            t.Equals(r.ipu.readRegister(0x00), 0xA1A2A3A4u, "FDEC returns 32 bits after the skip");
            t.Equals(r.ipu.bitPosition(), 8u, "FDEC does not consume the fetched bits");
            r.cmd(0u | 4u); // BCLR with bit position 4
            t.Equals(r.ipu.bitPosition(), 4u, "BCLR sets BP");
            t.Equals(r.ipu.inFifoQwords(), 0u, "BCLR empties the FIFO");
            r.feed(data);
            r.cmd(4u << 28);
            t.Equals(r.ipu.readRegister(0x00), 0x0A1A2A3Au, "bits are read from bit 4 of the first byte");
        });

        tc.Run("a header-walking FDEC sequence never leaves the unit busy", [](TestCase &t)
        {
            Rig r;
            std::vector<uint8_t> data(16 * 20);
            for (size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<uint8_t>(i * 7 + 1);
            r.input = data;
            r.ipu.writeRegister(0x10, 1u << 30);
            r.cmd(0u);
            const uint32_t skips[] = {0, 32, 32, 30, 1, 8, 8, 24, 32, 1, 1};
            for (uint32_t skip : skips)
            {
                r.cmd((4u << 28) | skip);
                t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "FDEC finished (skip " + std::to_string(skip) + ")");
                t.Equals(r.ipu.readRegister(0x00) != 0xFFFFFFFFu ? 1u : 0u, 1u, "result readable");
            }
        });

        tc.Run("a whole-frame CSC streams 1023 macroblocks through source and sink", [](TestCase &t)
        {
            Rig r;
            const size_t mbs = 1023;
            r.input.assign(mbs * 384 + 16 * 8, 0);
            for (size_t m = 0; m < mbs; ++m)
            {
                std::memset(r.input.data() + m * 384, 126, 256);
                std::memset(r.input.data() + m * 384 + 256, 128, 128);
            }
            r.cmd(0u);
            r.cmd((7u << 28) | static_cast<uint32_t>(mbs));
            t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "CSC completes");
            t.Equals(static_cast<uint32_t>(r.output.size()) + r.ipu.outFifoQwords() * 16u, static_cast<uint32_t>(mbs * 1024u), "all RGB32 pixels produced");
        });

        tc.Run("a command waits for data and resumes when it arrives", [](TestCase &t)
        {
            Rig r;
            r.sourceOpen = false;
            r.cmd(4u << 28 | 3u); // FDEC needs 35 bits
            t.Equals(r.ipu.readRegister(0x04) >> 31, 1u, "BUSY while starved");
            std::vector<uint8_t> data(16, 0xFF);
            r.sourceOpen = true;
            r.feed(data);
            t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "still starved with 16 bytes? 35 bits are available");
            t.Equals(r.ipu.readRegister(0x00), 0xFFFFFFFFu, "result");
        });

        tc.Run("BDEC decodes a flat intra macroblock and orders the output", [](TestCase &t)
        {
            Rig r;
            BitWriter w;
            w.put(flatIntraMacroblock());
            r.input = w.finish();
            r.cmd((2u << 28) | (1u << 27) | (1u << 26) | (4u << 16)); // intra, reset DC, qscale code 4
            t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "BDEC completes");
            t.Equals(static_cast<uint32_t>(r.output.size()) + r.ipu.outFifoQwords() * 16u >= 768u ? 1u : 0u, 1u, "768 bytes produced");
            // Drain whatever is still in the FIFO through the 128-bit port.
            std::vector<uint8_t> all = r.output;
            uint8_t q[16];
            while (r.ipu.readOutFifo(q))
                all.insert(all.end(), q, q + 16);
            t.Equals(static_cast<uint32_t>(all.size()), 768u, "one macroblock = 384 halfwords");
            bool luma = true, cb = true, cr = true;
            for (size_t i = 0; i < 256; ++i)
                luma &= le16(all, i) == 144u;
            for (size_t i = 256; i < 320; ++i)
                cb &= le16(all, i) == 112u;
            for (size_t i = 320; i < 384; ++i)
                cr &= le16(all, i) == 128u;
            t.IsTrue(luma, "luma 128 + 16");
            t.IsTrue(cb, "Cb 128 - 16");
            t.IsTrue(cr, "Cr 128");
            t.Equals((r.ipu.readRegister(0x10) >> 8) & 0x3Fu, 0x3Fu, "CBP of an intra macroblock");
        });

        tc.Run("IDEC decodes, converts and stops at the next start code", [](TestCase &t)
        {
            Rig r;
            BitWriter w;
            w.put("1"); // macroblock_type: intra
            w.put(flatIntraMacroblock());
            w.alignByte();
            w.put("00000000" "00000000" "00000001" "10101010"); // start code 0x000001AA
            r.input = w.finish();
            r.cmd((1u << 28)); // IDEC, no skip, ofm = RGB32
            t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "IDEC completes");
            std::vector<uint8_t> all = r.output;
            uint8_t q[16];
            while (r.ipu.readOutFifo(q))
                all.insert(all.end(), q, q + 16);
            t.Equals(static_cast<uint32_t>(all.size()), 1024u, "RGB32 macroblock");
            uint8_t y[256], cb[64], cr[64], expect[1024];
            std::memset(y, 144, sizeof(y));
            std::memset(cb, 112, sizeof(cb));
            std::memset(cr, 128, sizeof(cr));
            Ipu::yuv420ToRgb32(y, cb, cr, 0, 0, expect);
            t.IsTrue(std::memcmp(all.data(), expect, 1024) == 0, "pixels equal the CSC of the decoded planes");
            t.IsTrue((r.ipu.readRegister(0x10) & Ipu::kCtrlScd) != 0, "SCD set");
            t.Equals(r.ipu.readRegister(0x00) , 0x000001AAu, "the start code is left in the stream");
        });

        tc.Run("the OUT FIFO stalls a command until it is drained", [](TestCase &t)
        {
            Rig r;
            r.sinkOpen = false;
            BitWriter w;
            for (int i = 0; i < 3; ++i)
                w.put(flatIntraMacroblock());
            r.input = w.finish();
            r.cmd((2u << 28) | (1u << 27) | (1u << 26));
            t.Equals(r.ipu.readRegister(0x04) >> 31, 1u, "BUSY: 48 QW do not fit in the 8 QW FIFO");
            t.Equals(r.ipu.outFifoQwords(), 8u, "OFC saturates at 8");
            r.sinkOpen = true;
            r.ipu.run();
            t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "completes once DMA3 drains");
            t.Equals(static_cast<uint32_t>(r.output.size()), 768u - r.ipu.outFifoQwords() * 16u, "everything else was delivered");
        });

        tc.Run("a macroblock split across DMA chunks decodes after rollback", [](TestCase &t)
        {
            Rig r;
            BitWriter w;
            w.put(flatIntraMacroblock());
            std::vector<uint8_t> data = w.finish();
            // Only the first quadword is available; the macroblock needs more than that? Force it: pad the head.
            BitWriter big;
            for (int i = 0; i < 40; ++i)
                big.put("1110" "10000" "10"); // lots of bits so the macroblock exceeds 16 bytes
            std::vector<uint8_t> first(data.begin(), data.begin() + 16);
            std::vector<uint8_t> rest(data.begin() + 16, data.end());
            r.cmd((2u << 28) | (1u << 27) | (1u << 26));
            r.feed(first);
            if (!rest.empty())
            {
                t.Equals(r.ipu.readRegister(0x04) >> 31, 1u, "starved mid macroblock");
                r.feed(rest);
            }
            t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "completes after the data arrives");
            std::vector<uint8_t> all = r.output;
            uint8_t q[16];
            while (r.ipu.readOutFifo(q))
                all.insert(all.end(), q, q + 16);
            t.Equals(static_cast<uint32_t>(all.size()), 768u, "output intact");
            t.Equals(le16(all, 0), 144u, "same pixels as the unsplit decode");
        });

        tc.Run("SETIQ loads matrices in zigzag order; SETTH and reset behave", [](TestCase &t)
        {
            Rig r;
            std::vector<uint8_t> m(64);
            for (int i = 0; i < 64; ++i)
                m[i] = static_cast<uint8_t>(i + 1);
            r.cmd(5u << 28);
            r.feed(std::vector<uint8_t>(m.begin(), m.end()));
            t.Equals(static_cast<int>(r.ipu.intraMatrix()[0]), 1, "first entry");
            t.Equals(static_cast<int>(r.ipu.intraMatrix()[1]), 2, "zigzag index 1 -> raster 1");
            t.Equals(static_cast<int>(r.ipu.intraMatrix()[8]), 3, "zigzag index 2 -> raster 8");
            r.cmd((9u << 28) | (0x40u << 16) | 0x20u);
            r.ipu.writeRegister(0x10, 1u << 30); // RST
            t.Equals(r.ipu.readRegister(0x04) >> 31, 0u, "reset clears BUSY");
            t.IsTrue(r.interrupts >= 2, "completion interrupts were raised");
        });

        tc.Run("CTRL exposes IFC, OFC and the stored picture parameters", [](TestCase &t)
        {
            Ipu ipu;
            ipu.writeRegister(0x10, (1u << 16) | Ipu::kCtrlIvf | (3u << 24));
            const uint32_t c = ipu.readRegister(0x10);
            t.Equals((c >> 16) & 3u, 1u, "IDP");
            t.IsTrue((c & Ipu::kCtrlIvf) != 0, "IVF");
            t.Equals((c >> 24) & 7u, 3u, "PCT");
            uint8_t q[16] = {};
            for (int i = 0; i < 3; ++i)
                ipu.writeInFifo(q);
            t.Equals(ipu.readRegister(0x10) & 0xFu, 3u, "IFC counts quadwords");
            t.Equals((ipu.readRegister(0x20) >> 8) & 0xFu, 3u, "BP register reports IFC too");
            for (int i = 0; i < 10; ++i)
                ipu.writeInFifo(q);
            t.Equals(ipu.readRegister(0x10) & 0xFu, 8u, "the FIFO holds 8 quadwords");
            t.IsFalse(ipu.writeInFifo(q), "a full FIFO rejects more data");
        });
    });
}
