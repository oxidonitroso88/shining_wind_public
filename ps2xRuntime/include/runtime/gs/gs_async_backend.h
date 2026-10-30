#pragma once

#include "runtime/gs/gs_backend.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

// Runs the drawing commands of another GSRasterBackend on a worker thread, like the real GS runs in parallel with the
// EE. Commands that only produce side effects inside the backend (primitives, CLUT loads, host->local transfers,
// texture cache flushes) are queued and executed in order; every operation that observes the backend (presentation,
// reads, snapshots, FINISH, resets) waits until the queue is empty first, so the EE never sees stale pixels.
// PS2X_GS_SYNC=1 makes GSFrontend use the wrapped backend directly (no thread).
class GSAsyncBackend final : public GSRasterBackend
{
public:
    explicit GSAsyncBackend(std::unique_ptr<GSRasterBackend> inner);
    ~GSAsyncBackend() override;

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

private:
    struct ClutCommand
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };
    struct UploadCommand
    {
        std::vector<uint8_t> data;
    };
    struct TextureFlushCommand
    {
    };
    using Command = std::variant<GSPrimitiveBatch, ClutCommand, GSTransferCommand, UploadCommand, TextureFlushCommand>;

    void enqueue(Command command, size_t weightBytes);
    void drain() const;
    // Waits only for the commands enqueued before this call (a fence), so a presenter running next to a producer that
    // never lets the queue go empty cannot starve.
    void drainUpToFence() const;
    void workerLoop();
    void execute(Command &command);

    std::unique_ptr<GSRasterBackend> m_inner;

    mutable std::mutex m_mutex;
    mutable std::condition_variable m_workCv;  // worker waits for commands
    mutable std::condition_variable m_idleCv;  // producers wait for space / drain
    std::deque<Command> m_queue;
    size_t m_queuedBytes = 0;
    bool m_executing = false;
    uint64_t m_enqueuedCount = 0; // commands ever enqueued
    uint64_t m_completedCount = 0; // commands fully executed
    bool m_stop = false;
    std::thread m_thread;
};
