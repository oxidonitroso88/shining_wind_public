#include "runtime/gs/gs_async_backend.h"

#include <algorithm>
#include <utility>

namespace
{
    // Back-pressure: the EE may run ahead of the rasteriser by at most this much work.
    constexpr size_t kMaxQueuedCommands = 20000u;
    constexpr size_t kMaxQueuedBytes = 48u * 1024u * 1024u;
} // namespace

GSAsyncBackend::GSAsyncBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
    m_thread = std::thread([this]() { workerLoop(); });
}

GSAsyncBackend::~GSAsyncBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_workCv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void GSAsyncBackend::workerLoop()
{
    std::vector<Command> local;
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_workCv.wait(lock, [this]() { return m_stop || !m_queue.empty(); });
            if (m_queue.empty() && m_stop)
                return;
            // Take a bounded batch so producers blocked on back-pressure are released regularly.
            const size_t take = std::min<size_t>(m_queue.size(), 256u);
            for (size_t i = 0; i < take; ++i)
            {
                local.push_back(std::move(m_queue.front()));
                m_queue.pop_front();
            }
            m_executing = true;
        }
        size_t releasedBytes = 0;
        for (Command &command : local)
        {
            if (const auto *upload = std::get_if<UploadCommand>(&command))
                releasedBytes += upload->data.size();
            execute(command);
        }
        const size_t executed = local.size();
        local.clear();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_completedCount += executed;
            m_executing = false;
            m_queuedBytes = m_queuedBytes > releasedBytes ? m_queuedBytes - releasedBytes : 0u;
        }
        m_idleCv.notify_all();
    }
}

void GSAsyncBackend::execute(Command &command)
{
    struct Visitor
    {
        GSRasterBackend &inner;
        void operator()(const GSPrimitiveBatch &batch) const { inner.Submit(batch); }
        void operator()(const ClutCommand &clut) const { inner.LoadClut(clut.tex0, clut.texclut); }
        void operator()(const GSTransferCommand &transfer) const { inner.BeginTransfer(transfer); }
        void operator()(const UploadCommand &upload) const
        {
            inner.UploadImage(upload.data.data(), static_cast<uint32_t>(upload.data.size()));
        }
        void operator()(const TextureFlushCommand &) const { inner.TextureFlush(); }
    };
    std::visit(Visitor{*m_inner}, command);
}

void GSAsyncBackend::enqueue(Command command, size_t weightBytes)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idleCv.wait(lock, [this]() { return m_stop || (m_queue.size() < kMaxQueuedCommands && m_queuedBytes < kMaxQueuedBytes); });
    m_queue.push_back(std::move(command));
    ++m_enqueuedCount;
    m_queuedBytes += weightBytes;
    lock.unlock();
    m_workCv.notify_one();
}

void GSAsyncBackend::drain() const
{
    // Fence semantics: wait for everything enqueued before this call, not for an empty queue. With the producer (EE) and the
    // consumer of the drain (host presenter) on different threads, "queue empty" may never happen while the EE keeps drawing.
    drainUpToFence();
}

void GSAsyncBackend::drainUpToFence() const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const uint64_t target = m_enqueuedCount;
    m_idleCv.wait(lock, [this, target]() { return m_stop || m_completedCount >= target; });
}

void GSAsyncBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    drain();
    m_inner->Initialize(vram, vramSize);
}

void GSAsyncBackend::Reset()
{
    drain();
    m_inner->Reset();
}

void GSAsyncBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (batch.vertexCount == 0u)
        return;
    enqueue(Command{batch}, 0u);
}

void GSAsyncBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    enqueue(Command{ClutCommand{tex0, texclut}}, 0u);
}

void GSAsyncBackend::BeginTransfer(const GSTransferCommand &command)
{
    enqueue(Command{command}, 0u);
}

void GSAsyncBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    UploadCommand upload;
    upload.data.assign(data, data + sizeBytes);
    enqueue(Command{std::move(upload)}, sizeBytes);
}

void GSAsyncBackend::Flush()
{
    drain();
    m_inner->Flush();
}

void GSAsyncBackend::TextureFlush()
{
    enqueue(Command{TextureFlushCommand{}}, 0u);
}

void GSAsyncBackend::Sync(GSSyncReason reason)
{
    drain();
    m_inner->Sync(reason);
}

PresentationFrame GSAsyncBackend::Present(const GSPresentationRequest &request)
{
    drainUpToFence();
    return m_inner->Present(request);
}

bool GSAsyncBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    drain();
    return m_inner->ClearFramebuffer(context, rgba);
}

uint32_t GSAsyncBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    drain();
    return m_inner->ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSAsyncBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    drain();
    return m_inner->ReadVram(psm, base, bw, x, y);
}

void GSAsyncBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    drain();
    m_inner->WriteVram(psm, base, bw, x, y, value);
}

void GSAsyncBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    drain();
    m_inner->SnapshotVram(out);
}

GSTransferSnapshot GSAsyncBackend::GetTransferSnapshot() const
{
    drain();
    return m_inner->GetTransferSnapshot();
}
