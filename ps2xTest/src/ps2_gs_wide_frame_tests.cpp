// Presentation frames wider than the 640 pixel host frame (GPU scanout at a higher internal resolution, PS2X_GS_SCALE) are handed over tightly packed.
// Regression: the latch used a fixed 640 pixel row stride for every frame, so a 1280 pixel wide frame came out with odd rows from the right half and half
// the height (the "scene duplicated side by side" of the first high resolution scanout experiments).
#include "MiniTest.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace
{
    // Backend that does nothing but present a frame whose pixel (x, y) encodes its own coordinates.
    class WideFrameBackend final : public GSRasterBackend
    {
    public:
        WideFrameBackend(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}
        void Initialize(uint8_t *, uint32_t) override {}
        void Reset() override {}
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &) override {}
        void UploadImage(const uint8_t *, uint32_t) override {}
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}
        PresentationFrame Present(const GSPresentationRequest &) override
        {
            PresentationFrame frame;
            frame.width = m_width;
            frame.height = m_height;
            // like the real producers: rows of the CPU CRTC use the fixed 640 pixel stride, wider (GPU) frames are tightly packed
            const uint32_t stride = m_width > 640u ? m_width : 640u;
            frame.pixels.resize(static_cast<size_t>(stride) * m_height * 4u);
            for (uint32_t y = 0; y < m_height; ++y)
                for (uint32_t x = 0; x < m_width; ++x)
                {
                    uint8_t *p = frame.pixels.data() + (static_cast<size_t>(y) * stride + x) * 4u;
                    p[0] = static_cast<uint8_t>(x);
                    p[1] = static_cast<uint8_t>(x >> 8);
                    p[2] = static_cast<uint8_t>(y);
                    p[3] = 0xFF;
                }
            return frame;
        }
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return false; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override { return 0; }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0; }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void SnapshotVram(std::vector<uint8_t> &) const override {}
        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }

    private:
        uint32_t m_width, m_height;
    };

    // returns false if any pixel of the latched frame does not carry its own coordinates
    bool latchedFrameIsIntact(uint32_t width, uint32_t height, uint32_t &gotWidth, uint32_t &gotHeight)
    {
        std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
        GSRegisters regs{};
        regs.pmode = 1ull;
        GS gs;
        gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);
        gs.setRasterBackend(std::make_unique<WideFrameBackend>(width, height));
        gs.latchHostPresentationFrame();
        std::vector<uint8_t> pixels;
        gotWidth = gotHeight = 0u;
        if (!gs.copyLatchedHostPresentationFrame(pixels, gotWidth, gotHeight) || gotWidth != width || gotHeight != height)
            return false;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t *p = pixels.data() + (static_cast<size_t>(y) * width + x) * 4u;
                if (p[0] != static_cast<uint8_t>(x) || p[1] != static_cast<uint8_t>(x >> 8) || p[2] != static_cast<uint8_t>(y))
                    return false;
            }
        return true;
    }
}

void register_ps2_gs_wide_frame_tests()
{
    MiniTest::Case("PS2GSWideFrame", [](TestCase &tc)
    {
        tc.Run("a 1280x447 presentation frame (2x super-sampled scanout) reaches the window code unscrambled", [](TestCase &t)
        {
            uint32_t w = 0u, h = 0u;
            t.IsTrue(latchedFrameIsIntact(1280u, 447u, w, h), "every pixel of the wide frame must keep its coordinates");
        });
        tc.Run("640 pixel frames keep working (fixed 640 pixel stride of the CPU CRTC)", [](TestCase &t)
        {
            uint32_t w = 0u, h = 0u;
            t.IsTrue(latchedFrameIsIntact(640u, 448u, w, h), "native size frame");
            t.IsTrue(latchedFrameIsIntact(512u, 448u, w, h), "narrower than the host frame");
        });
    });
}
