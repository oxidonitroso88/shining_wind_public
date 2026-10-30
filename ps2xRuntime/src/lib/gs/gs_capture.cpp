#include "runtime/gs/gs_capture.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>

namespace
{
    static_assert(std::is_trivially_copyable_v<GSPrimitiveBatch>, "capture stores GSPrimitiveBatch raw");
    static_assert(std::is_trivially_copyable_v<GSTransferCommand>, "capture stores GSTransferCommand raw");
    static_assert(std::is_trivially_copyable_v<GSContext>, "capture stores GSContext raw");
    static_assert(std::is_trivially_copyable_v<GSTex0Reg> && std::is_trivially_copyable_v<GSTexClutReg>, "capture stores CLUT regs raw");

    class GSCaptureBackend final : public GSRasterBackend
    {
    public:
        GSCaptureBackend(std::unique_ptr<GSRasterBackend> inner, std::string path, double startSeconds, uint32_t presents)
            : m_inner(std::move(inner)), m_path(std::move(path)), m_start(startSeconds), m_presentsWanted(presents),
              m_epoch(std::chrono::steady_clock::now())
        {
        }
        ~GSCaptureBackend() override { finish(); }

        void Initialize(uint8_t *vram, uint32_t vramSize) override { m_inner->Initialize(vram, vramSize); }
        void Reset() override
        {
            record(GSCaptureRecord::Reset, nullptr, 0);
            m_inner->Reset();
        }
        void Submit(const GSPrimitiveBatch &batch) override
        {
            record(GSCaptureRecord::Submit, &batch, sizeof(batch));
            m_inner->Submit(batch);
        }
        void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override
        {
            struct Pair
            {
                GSTex0Reg a;
                GSTexClutReg b;
            } p{tex0, texclut};
            record(GSCaptureRecord::Clut, &p, sizeof(p));
            m_inner->LoadClut(tex0, texclut);
        }
        void BeginTransfer(const GSTransferCommand &command) override
        {
            record(GSCaptureRecord::Transfer, &command, sizeof(command));
            m_inner->BeginTransfer(command);
        }
        void UploadImage(const uint8_t *data, uint32_t sizeBytes) override
        {
            if (capturing())
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_file)
                {
                    const uint8_t type = static_cast<uint8_t>(GSCaptureRecord::Upload);
                    std::fwrite(&type, 1, 1, m_file);
                    std::fwrite(&sizeBytes, 4, 1, m_file);
                    std::fwrite(data, 1, sizeBytes, m_file);
                }
            }
            m_inner->UploadImage(data, sizeBytes);
        }
        void Flush() override { m_inner->Flush(); }
        void TextureFlush() override
        {
            record(GSCaptureRecord::TextureFlush, nullptr, 0);
            m_inner->TextureFlush();
        }
        void Sync(GSSyncReason reason) override { m_inner->Sync(reason); }
        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            record(GSCaptureRecord::Present, nullptr, 0);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_file && ++m_presents >= m_presentsWanted)
                    finishLocked();
            }
            return m_inner->Present(request);
        }
        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            struct Pair
            {
                GSContext a;
                uint32_t b;
            } p{context, rgba};
            record(GSCaptureRecord::Clear, &p, sizeof(p));
            return m_inner->ClearFramebuffer(context, rgba);
        }
        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            record(GSCaptureRecord::ConsumeLocal, &maxBytes, sizeof(maxBytes));
            return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
        }
        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            return m_inner->ReadVram(psm, base, bw, x, y);
        }
        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            const uint32_t words[6] = {psm, base, bw, x, y, value};
            record(GSCaptureRecord::WriteVram, words, sizeof(words));
            m_inner->WriteVram(psm, base, bw, x, y, value);
        }
        void SnapshotVram(std::vector<uint8_t> &out) const override { m_inner->SnapshotVram(out); }
        GSTransferSnapshot GetTransferSnapshot() const override { return m_inner->GetTransferSnapshot(); }

    private:
        bool capturing()
        {
            if (m_done)
                return false;
            if (m_file)
                return true;
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - m_epoch).count() < m_start)
                return false;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_file || m_done)
                return m_file != nullptr;
            std::vector<uint8_t> vram;
            m_inner->SnapshotVram(vram); // drains the worker: a consistent starting point
            m_file = std::fopen(m_path.c_str(), "wb");
            if (!m_file)
            {
                m_done = true;
                return false;
            }
            std::fwrite("GSCAP1", 1, 6, m_file);
            const uint32_t size = static_cast<uint32_t>(vram.size());
            std::fwrite(&size, 4, 1, m_file);
            std::fwrite(vram.data(), 1, vram.size(), m_file);
            std::fprintf(stderr, "[gs:capture] started %s (%u bytes of VRAM)\n", m_path.c_str(), size);
            return true;
        }
        void record(GSCaptureRecord type, const void *payload, size_t size)
        {
            if (!capturing())
                return;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_file)
                return;
            const uint8_t t = static_cast<uint8_t>(type);
            std::fwrite(&t, 1, 1, m_file);
            if (size)
                std::fwrite(payload, 1, size, m_file);
        }
        void finishLocked()
        {
            if (!m_file)
                return;
            std::fclose(m_file);
            m_file = nullptr;
            m_done = true;
            std::fprintf(stderr, "[gs:capture] finished after %u presents\n", m_presents);
        }
        void finish()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            finishLocked();
        }

        std::unique_ptr<GSRasterBackend> m_inner;
        std::string m_path;
        double m_start;
        uint32_t m_presentsWanted;
        std::chrono::steady_clock::time_point m_epoch;
        std::mutex m_mutex;
        FILE *m_file = nullptr;
        bool m_done = false;
        uint32_t m_presents = 0;
    };
} // namespace

std::unique_ptr<GSRasterBackend> gsMaybeWrapWithCapture(std::unique_ptr<GSRasterBackend> inner)
{
    const char *spec = std::getenv("PS2X_GS_CAPTURE");
    if (!spec || !*spec)
        return inner;
    std::string text(spec);
    const size_t a = text.find(';');
    const size_t b = a == std::string::npos ? a : text.find(';', a + 1);
    const std::string path = text.substr(0, a);
    const double start = a == std::string::npos ? 0.0 : std::atof(text.c_str() + a + 1);
    const uint32_t presents = b == std::string::npos ? 60u : static_cast<uint32_t>(std::atoi(text.c_str() + b + 1));
    return std::make_unique<GSCaptureBackend>(std::move(inner), path, start, presents);
}
