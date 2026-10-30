// ps2_gs_replay: replays a GSCAP1 capture (PS2X_GS_CAPTURE) through GSCpuBackend and reports timing plus a bit-exact hash of VRAM.
//   ps2_gs_replay <capture> [--repeat N] [--ppm out.ppm]
// Used to measure rasteriser optimisations on real game workloads and to prove they do not change a single VRAM bit.
#include "runtime/gs/gs_capture.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <atomic>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
    // --profile: samples this thread's RIP every ~1 ms and prints module-relative hot addresses (resolve with the /MAP file).
    struct RipSampler
    {
        std::atomic<bool> stop{false};
        std::thread th;
        std::unordered_map<uint64_t, uint32_t> hist;
        void start()
        {
#ifdef _WIN32
            HANDLE self = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
            const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
            th = std::thread([this, self, base]() {
                while (!stop.load())
                {
                    if (SuspendThread(self) != static_cast<DWORD>(-1))
                    {
                        CONTEXT c;
                        c.ContextFlags = CONTEXT_CONTROL;
                        const BOOL ok = GetThreadContext(self, &c);
                        ResumeThread(self);
                        if (ok && c.Rip >= base)
                            ++hist[c.Rip - base];
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
#endif
        }
        void finish()
        {
            stop.store(true);
            if (th.joinable())
                th.join();
            std::vector<std::pair<uint32_t, uint64_t>> rows;
            for (auto &kv : hist)
                rows.emplace_back(kv.second, kv.first);
            std::sort(rows.rbegin(), rows.rend());
            uint64_t total = 0;
            for (auto &r : rows)
                total += r.first;
            std::printf("[rip] samples=%llu\n", static_cast<unsigned long long>(total));
            for (size_t i = 0; i < rows.size() && i < 600; ++i)
                std::printf("[rip] rva=0x%llx n=%u\n", static_cast<unsigned long long>(rows[i].second), rows[i].first);
        }
    };

    struct Op
    {
        GSCaptureRecord type;
        size_t offset; // payload offset in `blob`
        uint32_t size;
    };

    uint64_t fnv1a(const uint8_t *data, size_t size)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= data[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    using Clock = std::chrono::steady_clock;
    double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
}

// --compare: decodes the frame targets listed in <targets.txt> (fbp fbw psm prims) from two raw VRAM dumps with the project's own
// pixel accessors and reports the per-channel differences. Tolerance counts a pixel as "close" when every channel differs by <= tol.
static int compareVram(int argc, char **argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: ps2_gs_replay --compare <a.bin> <b.bin> <targets.txt> [--ppm-prefix p] [--tol N]\n");
        return 2;
    }
    const char *prefix = nullptr;
    int tol = 2;
    int grid = 0;
    for (int i = 5; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--ppm-prefix") && i + 1 < argc) prefix = argv[++i];
        else if (!std::strcmp(argv[i], "--tol") && i + 1 < argc) tol = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--grid") && i + 1 < argc) grid = std::atoi(argv[++i]);
    }
    auto load = [](const char *path, std::vector<uint8_t> &out) {
        FILE *f = std::fopen(path, "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        out.resize(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    };
    std::vector<uint8_t> va, vb;
    if (!load(argv[2], va) || !load(argv[3], vb) || va.size() != vb.size() || va.empty())
    {
        std::fprintf(stderr, "cannot load the two VRAM dumps (or sizes differ)\n");
        return 2;
    }
    size_t rawDiff = 0;
    for (size_t i = 0; i < va.size(); ++i)
        rawDiff += va[i] != vb[i];
    std::printf("raw VRAM bytes differing: %zu of %zu (%.3f%%)\n", rawDiff, va.size(), 100.0 * static_cast<double>(rawDiff) / static_cast<double>(va.size()));
    GSCpuBackend A, B;
    A.Initialize(va.data(), static_cast<uint32_t>(va.size()));
    B.Initialize(vb.data(), static_cast<uint32_t>(vb.size()));
    FILE *tf = std::fopen(argv[4], "r");
    if (!tf)
        return 2;
    unsigned fbp, fbw, psm;
    unsigned long long prims;
    while (std::fscanf(tf, "%u %u %u %llu", &fbp, &fbw, &psm, &prims) == 4)
    {
        const uint32_t width = std::min<uint32_t>(std::max<uint32_t>(fbw, 1u) * 64u, 640u), height = 448u;
        const uint32_t block = GSInternal::framePageBaseToBlock(fbp);
        uint64_t differ = 0, close = 0, nonBlack = 0;
        double sumAbs[4] = {0, 0, 0, 0}, sq = 0;
        int maxD[4] = {0, 0, 0, 0};
        std::vector<uint8_t> diffImg;
        const uint32_t gcols = grid > 0 ? (width + static_cast<uint32_t>(grid) - 1) / static_cast<uint32_t>(grid) : 0u, grows = grid > 0 ? (height + static_cast<uint32_t>(grid) - 1) / static_cast<uint32_t>(grid) : 0u;
        std::vector<uint64_t> cellExact(gcols * grows, 0), cellClose(gcols * grows, 0), cellN(gcols * grows, 0);
        std::vector<int> cellMax(gcols * grows, 0);
        if (prefix) diffImg.resize(static_cast<size_t>(width) * height * 3);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t pa = A.ReadVram(psm, block, std::max<uint32_t>(fbw, 1u), x, y);
                const uint32_t pb = B.ReadVram(psm, block, std::max<uint32_t>(fbw, 1u), x, y);
                if (pa & 0x00FFFFFFu) ++nonBlack;
                int worst = 0;
                for (int c = 0; c < 4; ++c)
                {
                    const int d = std::abs(static_cast<int>((pa >> (8 * c)) & 255u) - static_cast<int>((pb >> (8 * c)) & 255u));
                    sumAbs[c] += d;
                    maxD[c] = std::max(maxD[c], d);
                    if (c < 3) sq += static_cast<double>(d) * d;
                    worst = std::max(worst, d);
                }
                differ += pa != pb;
                if (grid > 0)
                {
                    const size_t ci = static_cast<size_t>(y / static_cast<uint32_t>(grid)) * gcols + x / static_cast<uint32_t>(grid);
                    ++cellN[ci];
                    cellExact[ci] += pa == pb;
                    cellClose[ci] += worst <= tol;
                    cellMax[ci] = std::max(cellMax[ci], worst);
                }
                close += worst <= tol;
                if (prefix)
                {
                    const uint8_t v = static_cast<uint8_t>(std::min(255, worst * 4));
                    uint8_t *o = &diffImg[(static_cast<size_t>(y) * width + x) * 3];
                    o[0] = v; o[1] = v; o[2] = v;
                }
            }
        const double n = static_cast<double>(width) * height;
        const double mse = sq / (n * 3.0);
        std::printf("target fbp=%u fbw=%u psm=%u (%llu prims) %ux%u: non-black(A) %.1f%%, identical %.2f%%, within +-%d %.2f%%, mean|d| R%.2f G%.2f B%.2f A%.2f, max R%d G%d B%d A%d, PSNR(RGB) %.1f dB\n",
                    fbp, fbw, psm, prims, width, height, 100.0 * static_cast<double>(nonBlack) / n, 100.0 * (n - static_cast<double>(differ)) / n, tol,
                    100.0 * static_cast<double>(close) / n, sumAbs[0] / n, sumAbs[1] / n, sumAbs[2] / n, sumAbs[3] / n, maxD[0], maxD[1], maxD[2], maxD[3],
                    mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0);
        if (grid > 0 && fbp == 0)
        {
            for (uint32_t r = 0; r < grows; ++r)
                for (uint32_t c = 0; c < gcols; ++c)
                {
                    const size_t ci = static_cast<size_t>(r) * gcols + c;
                    if (cellMax[ci] > 0)
                        std::printf("  cell %u (row %u col %u): identical %.1f%%, within +-%d %.1f%%, max diff %d\n", r * gcols + c, r, c, 100.0 * static_cast<double>(cellExact[ci]) / static_cast<double>(cellN[ci]), tol, 100.0 * static_cast<double>(cellClose[ci]) / static_cast<double>(cellN[ci]), cellMax[ci]);
                    else
                        std::printf("  cell %u (row %u col %u): identical 100%%\n", r * gcols + c, r, c);
                }
        }
        if (prefix)
        {
            for (int which = 0; which < 3; ++which)
            {
                char path[512];
                std::snprintf(path, sizeof(path), "%s_fbp%u_%s.ppm", prefix, fbp, which == 0 ? "a" : which == 1 ? "b" : "diff");
                if (FILE *o = std::fopen(path, "wb"))
                {
                    std::fprintf(o, "P6\n%u %u\n255\n", width, height);
                    for (uint32_t y = 0; y < height; ++y)
                        for (uint32_t x = 0; x < width; ++x)
                        {
                            uint8_t rgb[3];
                            if (which == 2)
                                std::memcpy(rgb, &diffImg[(static_cast<size_t>(y) * width + x) * 3], 3);
                            else
                            {
                                const uint32_t px = (which == 0 ? A : B).ReadVram(psm, block, std::max<uint32_t>(fbw, 1u), x, y);
                                rgb[0] = static_cast<uint8_t>(px & 255u); rgb[1] = static_cast<uint8_t>((px >> 8) & 255u); rgb[2] = static_cast<uint8_t>((px >> 16) & 255u);
                            }
                            std::fwrite(rgb, 1, 3, o);
                        }
                    std::fclose(o);
                }
            }
        }
    }
    std::fclose(tf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !std::strcmp(argv[1], "--compare"))
        return compareVram(argc, argv);
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: ps2_gs_replay <capture> [--repeat N] [--ppm out.ppm] [--dump-vram out.bin]\n"
                             "       ps2_gs_replay --compare <a.bin> <b.bin> <targets.txt> [--ppm-prefix p] [--tol N]\n");
        return 2;
    }
    int repeat = 1;
    const char *ppmPath = nullptr;
    const char *dumpVramPath = nullptr;
    double paceMs = 0;
    bool profile = false;
    size_t maxFrames = ~static_cast<size_t>(0);
    for (int i = 2; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc)
            repeat = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--ppm") && i + 1 < argc)
            ppmPath = argv[++i];
        else if (!std::strcmp(argv[i], "--dump-vram") && i + 1 < argc)
            dumpVramPath = argv[++i];
        else if (!std::strcmp(argv[i], "--pace-ms") && i + 1 < argc)
            paceMs = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--profile"))
            profile = true;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
            maxFrames = static_cast<size_t>(std::atoi(argv[++i]));
    }

    FILE *f = std::fopen(argv[1], "rb");
    if (!f)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    char magic[6];
    uint32_t vramSize = 0;
    if (std::fread(magic, 1, 6, f) != 6 || std::memcmp(magic, "GSCAP1", 6) != 0 || std::fread(&vramSize, 4, 1, f) != 1)
    {
        std::fprintf(stderr, "bad capture header\n");
        return 2;
    }
    std::vector<uint8_t> snapshot(vramSize);
    if (std::fread(snapshot.data(), 1, vramSize, f) != vramSize)
        return 2;

    std::vector<uint8_t> blob;
    std::vector<Op> ops;
    auto readBytes = [&](size_t n, size_t &offset) -> bool
    {
        offset = blob.size();
        blob.resize(offset + n);
        return n == 0 || std::fread(blob.data() + offset, 1, n, f) == n;
    };
    for (;;)
    {
        uint8_t t = 0;
        if (std::fread(&t, 1, 1, f) != 1)
            break;
        Op op{static_cast<GSCaptureRecord>(t), 0, 0};
        size_t fixed = 0;
        switch (op.type)
        {
        case GSCaptureRecord::Submit: fixed = sizeof(GSPrimitiveBatch); break;
        case GSCaptureRecord::Clut: fixed = sizeof(GSTex0Reg) + sizeof(GSTexClutReg); break;
        case GSCaptureRecord::Transfer: fixed = sizeof(GSTransferCommand); break;
        case GSCaptureRecord::Clear: fixed = sizeof(GSContext) + sizeof(uint32_t); break;
        case GSCaptureRecord::WriteVram: fixed = 6 * sizeof(uint32_t); break;
        case GSCaptureRecord::ConsumeLocal: fixed = sizeof(uint32_t); break;
        case GSCaptureRecord::Upload:
        {
            uint32_t size = 0;
            if (std::fread(&size, 4, 1, f) != 1)
                return 2;
            fixed = size;
            break;
        }
        case GSCaptureRecord::TextureFlush:
        case GSCaptureRecord::Present:
        case GSCaptureRecord::Reset:
            break;
        default:
            std::fprintf(stderr, "unknown record type %u\n", t);
            return 2;
        }
        op.size = static_cast<uint32_t>(fixed);
        if (!readBytes(fixed, op.offset))
        {
            std::fprintf(stderr, "truncated capture: ignoring the last partial record\n");
            break;
        }
        ops.push_back(op);
    }
    std::fclose(f);

    if (std::getenv("GS_REPLAY_STATS"))
    {
        std::map<std::string, std::pair<size_t, double>> hist;
        for (const Op &op : ops)
        {
            if (op.type != GSCaptureRecord::Submit)
                continue;
            GSPrimitiveBatch b;
            std::memcpy(&b, blob.data() + op.offset, sizeof(b));
            const auto &c = b.state.context;
            const double w = std::abs(b.vertices[1].x - b.vertices[0].x) * std::abs(b.vertices[1].y - b.vertices[0].y);
            char key[256];
            std::snprintf(key, sizeof(key), "type=%u tme=%u abe=%u iip=%u fst=%u fpsm=%02x fbmsk=%08x tpsm=%02x tfx=%u tcc=%u cpsm=%02x csm=%u lin=%u ztest=%llu zmsk=%u atst=%llu date=%llu pabe=%u fge=%u",
                          b.state.prim.type, b.state.prim.tme, b.state.prim.abe, b.state.prim.iip, b.state.prim.fst, c.frame.psm, c.frame.fbmsk,
                          c.tex0.psm, c.tex0.tfx, c.tex0.tcc, c.tex0.cpsm, c.tex0.csm, b.state.linearFilter ? 1u : 0u,
                          static_cast<unsigned long long>((c.test >> 17) & 3u), c.zbuf.zmask, static_cast<unsigned long long>(c.test & 0xFu),
                          static_cast<unsigned long long>((c.test >> 14) & 1u), b.state.pabe ? 1u : 0u, b.state.prim.fge);
            auto &h = hist[key];
            ++h.first;
            h.second += w;
        }
        std::vector<std::pair<std::string, std::pair<size_t, double>>> rows(hist.begin(), hist.end());
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.second > b.second.second; });
        for (size_t i = 0; i < rows.size() && i < 25; ++i)
            std::printf("[state] %8zu prims %12.0f px  %s\n", rows[i].second.first, rows[i].second.second, rows[i].first.c_str());
    }

    size_t submits = 0, frames = 0, uploads = 0;
    std::map<std::string, size_t> kinds;
    for (const Op &op : ops)
    {
        submits += op.type == GSCaptureRecord::Submit;
        frames += op.type == GSCaptureRecord::Present;
        uploads += op.type == GSCaptureRecord::Upload;
    }
    std::printf("capture: %zu records, %zu primitives, %zu uploads, %zu presents, VRAM %u bytes\n", ops.size(), submits, uploads, frames, vramSize);

    GSCpuBackend backend;
    std::vector<uint8_t> vram(vramSize);
    uint64_t hash = 0;
    GSFrameReg lastFrame{};
    bool haveFrame = false;
    double bestTotal = 1e300;
    RipSampler sampler;
    if (profile)
        sampler.start();
    for (int run = 0; run < repeat; ++run)
    {
        vram = snapshot;
        backend.Initialize(vram.data(), vramSize);
        backend.Reset();
        std::map<std::string, std::pair<size_t, double>> perKind;
        std::vector<double> frameMs;
        const auto runStart = Clock::now();
        auto frameStart = runStart;
        std::vector<uint8_t> sink(1u << 16);
        for (const Op &op : ops)
        {
            const uint8_t *p = blob.data() + op.offset;
            switch (op.type)
            {
            case GSCaptureRecord::Submit:
            {
                GSPrimitiveBatch batch;
                std::memcpy(&batch, p, sizeof(batch));
                const auto t0 = Clock::now();
                backend.Submit(batch);
                const double dt = ms(Clock::now() - t0);
                char name[64];
                std::snprintf(name, sizeof(name), "prim%u tme=%u abe=%u", batch.state.prim.type, batch.state.prim.tme, batch.state.prim.abe);
                auto &k = perKind[name];
                ++k.first;
                k.second += dt;
                lastFrame = batch.state.context.frame;
                haveFrame = true;
                break;
            }
            case GSCaptureRecord::Clut:
            {
                GSTex0Reg a;
                GSTexClutReg b;
                std::memcpy(&a, p, sizeof(a));
                std::memcpy(&b, p + sizeof(a), sizeof(b));
                backend.LoadClut(a, b);
                break;
            }
            case GSCaptureRecord::Transfer:
            {
                GSTransferCommand c;
                std::memcpy(&c, p, sizeof(c));
                backend.BeginTransfer(c);
                break;
            }
            case GSCaptureRecord::Upload:
                backend.UploadImage(p, op.size);
                break;
            case GSCaptureRecord::TextureFlush:
                backend.TextureFlush();
                break;
            case GSCaptureRecord::Clear:
            {
                GSContext c;
                uint32_t rgba;
                std::memcpy(&c, p, sizeof(c));
                std::memcpy(&rgba, p + sizeof(c), sizeof(rgba));
                backend.ClearFramebuffer(c, rgba);
                break;
            }
            case GSCaptureRecord::WriteVram:
            {
                uint32_t w[6];
                std::memcpy(w, p, sizeof(w));
                backend.WriteVram(w[0], w[1], w[2], w[3], w[4], w[5]);
                break;
            }
            case GSCaptureRecord::ConsumeLocal:
            {
                uint32_t maxBytes;
                std::memcpy(&maxBytes, p, sizeof(maxBytes));
                backend.ConsumeLocalToHostBytes(sink.data(), std::min<uint32_t>(maxBytes, static_cast<uint32_t>(sink.size())));
                break;
            }
            case GSCaptureRecord::Reset:
                backend.Reset();
                break;
            case GSCaptureRecord::Present:
            {
                const auto now = Clock::now();
                frameMs.push_back(ms(now - frameStart));
                frameStart = now;
                if (paceMs > 0)
                    std::this_thread::sleep_until(runStart + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(paceMs * static_cast<double>(frameMs.size()))));
                break;
            }
            }
            if (frameMs.size() >= maxFrames)
                break;
        }
        const double total = ms(Clock::now() - runStart);
        bestTotal = std::min(bestTotal, total);
        hash = fnv1a(vram.data(), vram.size());
        std::sort(frameMs.begin(), frameMs.end());
        const double avg = frameMs.empty() ? 0.0 : total / static_cast<double>(frameMs.size());
        std::printf("run %d: total %.1f ms, %zu frames, avg %.2f ms/frame (%.1f fps), median %.2f, p95 %.2f, max %.2f, VRAM hash %016llx\n",
                    run, total, frameMs.size(), avg, avg > 0 ? 1000.0 / avg : 0.0,
                    frameMs.empty() ? 0.0 : frameMs[frameMs.size() / 2],
                    frameMs.empty() ? 0.0 : frameMs[std::min(frameMs.size() - 1, frameMs.size() * 95 / 100)],
                    frameMs.empty() ? 0.0 : frameMs.back(), static_cast<unsigned long long>(hash));
        if (run == repeat - 1)
        {
            std::vector<std::pair<std::string, std::pair<size_t, double>>> rows(perKind.begin(), perKind.end());
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.second > b.second.second; });
            for (const auto &r : rows)
                std::printf("  %-24s %7zu prims %9.1f ms (%.4f ms each)\n", r.first.c_str(), r.second.first, r.second.second, r.second.second / static_cast<double>(r.second.first));
        }
    }
    if (profile)
        sampler.finish();
    {
        const auto &st = backend.stats();
        std::printf("raster stats (all runs): bbox %llu covered %llu written %llu\n", (unsigned long long)st.bboxPixels, (unsigned long long)st.coveredPixels, (unsigned long long)st.writtenPixels);
    }
    std::printf("best total %.1f ms\nFINAL_VRAM_HASH %016llx\n", bestTotal, static_cast<unsigned long long>(hash));

    if (dumpVramPath)
    {
        if (FILE *o = std::fopen(dumpVramPath, "wb"))
        {
            std::fwrite(vram.data(), 1, vram.size(), o);
            std::fclose(o);
        }
    }
    if (ppmPath && haveFrame)
    {
        const uint32_t width = 640u, height = 448u;
        FILE *out = std::fopen(ppmPath, "wb");
        if (out)
        {
            std::fprintf(out, "P6\n%u %u\n255\n", width, height);
            const uint32_t fbp = GSInternal::framePageBaseToBlock(lastFrame.fbp);
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t px = backend.ReadVram(lastFrame.psm, fbp, std::max<uint32_t>(lastFrame.fbw, 1u), x, y);
                    const uint8_t rgb[3] = {static_cast<uint8_t>(px & 0xFF), static_cast<uint8_t>((px >> 8) & 0xFF), static_cast<uint8_t>((px >> 16) & 0xFF)};
                    std::fwrite(rgb, 1, 3, out);
                }
            std::fclose(out);
        }
    }
    return 0;
}
