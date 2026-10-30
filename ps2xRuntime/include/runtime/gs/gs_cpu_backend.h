#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"
#include "runtime/gs/gs_raster_pool.h"

#include <array>
#include <memory>
#include <mutex>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // Diagnostics (replay tool): pixels visited by the rasteriser loops, pixels inside the primitive, pixels written.
    struct RasterStats
    {
        uint64_t bboxPixels = 0, coveredPixels = 0, writtenPixels = 0, texturedSamples = 0;
    };
    const RasterStats &stats() const { return m_stats; }

private:
    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    // Per-primitive decoded state (computed once in DrawPrimitive instead of per pixel).
    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    struct PixelSetup
    {
        bool fast = false; // no DATE/fog/FBMSK: the common case, handled by WritePixelFast (alpha test included)
        bool ate = false;  // alpha test that can reject pixels
        uint8_t atst = 1, aref = 0, afail = 0;
        int scX0 = 0, scY0 = 0, scX1 = 0, scY1 = 0;
        uint32_t fbp = 0, fbw = 1, fpsm = 0, zbp = 0, zpsm = 0;
        bool fb16 = false, fb24 = false;
        uint32_t ztest = 1;
        bool zwrite = true;
        bool abe = false, pabe = false, fba = false;
        bool stdBlend = false; // Cs*As + Cd*(1-As): A=Cs, B=Cd, C=As, D=Cd
        uint8_t asel = 0, bsel = 0, csel = 0, dsel = 0, fix = 0;
        ReadVramFunc readFb = nullptr, readZ = nullptr;
        WriteVramFunc writeFb = nullptr, writeZ = nullptr;
    };
    enum class TexKind : uint8_t { Indexed, Direct32, Other };
    struct TexSetup
    {
        bool valid = false;
        TexKind kind = TexKind::Other;
        uint32_t psm = 0, tbp0 = 0, tbw = 0;
        int texW = 1, texH = 1;
        uint8_t wrapU = 0, wrapV = 0;
        uint16_t minU = 0, maxU = 0, minV = 0, maxV = 0;
        GSTexaReg texa{};
    };
    struct TexLutKey
    {
        bool valid = false;
        uint32_t version = 0;
        uint8_t cpsm = 0, csm = 0, csa = 0, psm = 0, aem = 0, ta0 = 0, ta1 = 0;
    };
    // State owned by one drawing thread: its texture page cache and the last bilinear texel quad.
    struct RasterCtx
    {
        GSMem::TexturePageCache *cache = nullptr;
        bool quadValid = false;
        int quadU = 0, quadV = 0;
        uint32_t quad[4] = {0, 0, 0, 0};
    };
    template <typename Fn>
    void RunRows(int yBegin, int yEndInclusive, uint64_t pixels, Fn &&fn);
    bool EnsureRasterPool();
    void SetupPixelPipeline(const GSDrawState &state);
    void SetupTexture(const GSDrawState &state);
    void WritePixelFast(int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void WritePixelSlow(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(RasterCtx &rc, const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    GSMem::TexturePageCache m_texturePageCache;
    RasterStats m_stats;
    PixelSetup m_px;
    TexSetup m_tx;
    uint32_t m_clutVersion = 1;
    std::array<uint32_t, 256> m_texLut{};
    TexLutKey m_texLutKey;
    // Band-parallel drawing (PS2X_GS_THREADS=<helpers>, 0 disables): contexts[0] belongs to the calling thread.
    std::unique_ptr<GSRasterPool> m_pool;
    bool m_poolChecked = false;
    std::vector<RasterCtx> m_ctxs;
    std::vector<std::unique_ptr<GSMem::TexturePageCache>> m_helperCaches;

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
