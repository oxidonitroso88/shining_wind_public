#pragma once

#include "ps2x/iop/cdvd_device.h"
#include "ps2x/iop/iop_types.h"
#include <cstdint>
#include <memory>
#include <optional>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopCdvd
    {
    public:
        struct CompletionCallback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            uint32_t reason = 0u;
        };

        IopCdvd(IopHost &host, IopMemory &memory, IopKernel &kernel);
        ~IopCdvd();

        IopCdvd(const IopCdvd &) = delete;
        IopCdvd &operator=(const IopCdvd &) = delete;

        void reset() noexcept;
        [[nodiscard]] CdvdDevice::Snapshot deviceSnapshot() const;
        [[nodiscard]] bool hasControlServer(uint32_t sid) const;
        [[nodiscard]] RpcResult handleControlRpc(const RpcRequest &request);

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] std::optional<CompletionCallback> takeCompletionCallback() noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
