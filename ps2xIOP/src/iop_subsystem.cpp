#include "ps2x/iop/iop_subsystem.h"

#include "iop_service.h"
#include "iop_module_manager.h"
#include "emulator/iop_emulator.h"
#include "module_factories.h"
#include "ps2x/iop/ps2_path.h"
#include "fileio_rpc.h"
#include "sif_packet_adapter.h"

#include <array>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ps2x::iop
{
    namespace
    {
        constexpr uint32_t kLoadFileSid = 0x80000006u;
        constexpr uint32_t kLoadFileGetVersion = 0xFFu;
        // LF_F_GET_VERSION answer of the LOADFILE module in IOPRP300.IMG: its data holds
        // the ASCII text "3000" (PS2SDK's own module answers "3100"), returned verbatim.
        constexpr uint32_t kLoadFileVersion = 0x30303033u;
    }

    class IopSubsystem::Impl
    {
    public:
        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              emulator(hostRef), packetAdapter(hostRef), fileio(hostRef)
        {
            coreServices.emplace_back(detail::createMcservService(host));
            coreServices.emplace_back(detail::createDbcmanService(host));
            coreServices.emplace_back(detail::createLibSdService(host));
            refreshServiceModuleKeys();
            rebuildRoutes();
        }

        bool serviceActive(const detail::IopService &service) const
        {
            return moduleManager.isLoaded(service.moduleAliases());
        }

        void refreshServiceModuleKeys()
        {
            std::vector<std::string> keys;
            for (const auto &service : coreServices)
            {
                for (std::string_view alias : service->moduleAliases())
                    keys.emplace_back(alias);
            }
            moduleManager.setServiceModuleKeys(std::move(keys));
        }

        void rebuildRoutes()
        {
            routes.clear();
            lastError.clear();
            for (const auto &service : coreServices)
            {
                if (!serviceActive(*service))
                    continue;
                for (const uint32_t sid : service->sids())
                {
                    if (!routes.emplace(sid, service.get()).second)
                    {
                        std::ostringstream out;
                        out << "duplicate IOP SID 0x" << std::hex << sid << " in core services";
                        lastError = out.str();
                        routes.clear();
                        return;
                    }
                }
            }
        }

        void observeBufferLoad(const ModuleLoadResult &loaded)
        {
            if (loaded.moduleId <= 0)
                return;
            const std::string name = emulator.moduleIrxName(loaded.moduleId);
            if (name.empty())
                return;
            moduleManager.observePhysicalLoad(loaded.moduleId, name);
            rebuildRoutes();
        }

        void recordLoadOutcome(std::string_view path, bool hle)
        {
            constexpr size_t maxOutcomes = 32u;
            if (loadOutcomes.size() >= maxOutcomes || !loggedLoadPaths.emplace(path).second)
                return;
            std::string message = hle ? "[IOP:HLE] fallback module='" : "[IOP:load-failed] module='";
            message.append(path);
            message += hle ? "' physical IRX unavailable; using registered HLE provider"
                           : "' no HLE provider accepted the module; physical IRX was not loaded";
            loadOutcomes.push_back(message);
            host.log(hle ? LogLevel::Info : LogLevel::Warning, message);
        }

        IopHost &host;
        detail::ServiceList coreServices;
        std::unordered_map<uint32_t, detail::IopService *> routes;
        std::vector<std::string> loadOutcomes;
        std::unordered_set<std::string> loggedLoadPaths;
        std::string lastError;
        detail::IopModuleManager moduleManager;
        detail::IopEmulator emulator;
        detail::SifPacketAdapter packetAdapter;
        detail::FileIoServer fileio;
        // EE cycles left until a rebooted IOP raises its SMFLAG boot bits.
        uint64_t bootCyclesLeft = 0u;
        bool bootPending = false;
    };

    IopSubsystem::IopSubsystem(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopSubsystem::~IopSubsystem() = default;
    IopSubsystem::IopSubsystem(IopSubsystem &&) noexcept = default;
    IopSubsystem &IopSubsystem::operator=(IopSubsystem &&) noexcept = default;

    CdvdDevice::Snapshot IopSubsystem::cdvdSnapshot() const
    {
        return m_impl->emulator.cdvdSnapshot();
    }

    void IopSubsystem::reset()
    {
        m_impl->bootPending = false;
        m_impl->bootCyclesLeft = 0u;
        m_impl->packetAdapter.reset();
        m_impl->fileio.reset();
        m_impl->moduleManager.reset();
        m_impl->loadOutcomes.clear();
        m_impl->loggedLoadPaths.clear();
        for (auto &service : m_impl->coreServices)
        {
            if (service)
            {
                service->reset();
            }
        }
        m_impl->emulator.reset();
        m_impl->refreshServiceModuleKeys();
        m_impl->rebuildRoutes();
    }

    ModuleLoadResult IopSubsystem::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        const ParsedPs2Path parsed = parsePs2Path(path);
        if (!parsed)
            return {true, -1, -1};

        if (parsed.device != Ps2PathDevice::Rom0)
        {
            ModuleLoadResult physical = m_impl->emulator.loadModule(path, arguments, argumentSize);
            if (physical.moduleId > 0)
            {
                m_impl->moduleManager.observePhysicalLoad(physical.moduleId, path);
                m_impl->rebuildRoutes();
                return physical;
            }
        }

        ModuleLoadResult hle = m_impl->moduleManager.loadHle(path);
        if (hle.moduleId > 0)
        {
            m_impl->rebuildRoutes();
            if (parsed.device != Ps2PathDevice::Rom0)
                m_impl->recordLoadOutcome(path, true);
        }
        else
        {
            m_impl->recordLoadOutcome(path, false);
        }
        return hle;
    }

    // Modules loaded from a memory buffer have no path, so the loaded-module registry (which decides
    // whether an HLE service such as MCSERV is active) is keyed by the name inside the IRX.
    ModuleLoadResult IopSubsystem::loadModuleFromIopMemory(uint32_t iopAddress, const void *arguments, uint32_t argumentSize)
    {
        const ModuleLoadResult loaded = m_impl->emulator.loadModuleFromIopMemory(iopAddress, arguments, argumentSize);
        m_impl->observeBufferLoad(loaded);
        return loaded;
    }

    ModuleLoadResult IopSubsystem::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        const ModuleLoadResult loaded = m_impl->emulator.loadModuleBuffer(guestAddress, arguments, argumentSize);
        m_impl->observeBufferLoad(loaded);
        return loaded;
    }

    bool IopSubsystem::stopModule(int32_t moduleId, int32_t *result)
    {
        if (m_impl->moduleManager.stopHle(moduleId, result))
        {
            m_impl->rebuildRoutes();
            return true;
        }
        if (!m_impl->emulator.stopModule(moduleId, result))
            return false;
        m_impl->moduleManager.observePhysicalStop(moduleId);
        m_impl->rebuildRoutes();
        return true;
    }

    void IopSubsystem::runEeCycles(uint64_t eeCycles) noexcept
    {
        m_impl->emulator.runEeCycles(eeCycles);
        m_impl->packetAdapter.advance(eeCycles);
        m_impl->packetAdapter.pump();
        if (m_impl->bootPending)
        {
            if (eeCycles < m_impl->bootCyclesLeft)
            {
                m_impl->bootCyclesLeft -= eeCycles;
            }
            else
            {
                m_impl->bootPending = false;
                m_impl->bootCyclesLeft = 0u;
                m_impl->host.log(LogLevel::Info, "[SIF:wire] IOP reboot complete: SIFINIT|CMDINIT|BOOTEND");
                m_impl->host.notifyIopBoot(sif::StatSifInit | sif::StatCmdInit | sif::StatBootEnd);
            }
        }
    }

    RpcAbi IopSubsystem::selectRpcAbi(const RpcAbiRequest &request) const
    {
        for (const auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                const RpcAbi selected = service->selectRpcAbi(request);
                if (selected != RpcAbi::RuntimeDefault)
                {
                    return selected;
                }
            }
        }
        return RpcAbi::RuntimeDefault;
    }

    bool IopSubsystem::canBindRpc(uint32_t sid) const noexcept
    {
        if (sid == kLoadFileSid || sid == detail::kFileIoSid)
        {
            return true;
        }
        if (m_impl->routes.find(sid) != m_impl->routes.end())
        {
            return true;
        }
        return m_impl->emulator.hasRpcServer(sid);
    }

    RpcResult IopSubsystem::handleLoadFileRpc(const RpcRequest &request)
    {
        // LOADFILE (PS2SDK loadfile.c, LF_F_MOD_LOAD): the SDK sends a 512 byte
        // _lf_module_load_arg {arg_len, modres, path[252], args[252]} and reads back
        // {result, modres}: result is the module id (negative on failure), modres
        // the value returned by the module's start routine. Other LOADFILE
        // functions have no demonstrated consumer yet and stay uncompleted.
        RpcResult result{};
        result.handled = true;
        if (request.function == kLoadFileGetVersion && request.receive.size == 4u && request.receive.address != 0u)
        {
            const std::array<uint8_t, 4> version{uint8_t(kLoadFileVersion), uint8_t(kLoadFileVersion >> 8),
                                                 uint8_t(kLoadFileVersion >> 16), uint8_t(kLoadFileVersion >> 24)};
            if (!m_impl->host.writeGuest(request.receive.address, version.data(), version.size()))
                return result;
            result.resultAddress = request.receive.address;
            result.packetCompleted = true;
            return result;
        }
        constexpr uint32_t kArgSize = 512u, kPathOffset = 8u, kPathMax = 252u, kArgsOffset = 260u, kArgsMax = 252u;
        if (request.function == 6u && request.send.size == kArgSize && request.receive.size == 8u &&
            request.send.address != 0u && request.receive.address != 0u)
        {
            // LF_F_MOD_BUF_LOAD (_SifLoadModuleBuffer): {ptr, arg_len, unused[252], args[252]}. ptr is an IOP RAM
            // address the SDK filled beforehand (SifAllocIopHeap + SifDmaTransfer, see SifExecModuleBuffer).
            std::array<uint8_t, kArgSize> buffer{};
            if (!m_impl->host.readGuest(request.send.address, buffer.data(), buffer.size()))
                return result;
            auto word = [&](size_t at) { return uint32_t(buffer[at]) | (uint32_t(buffer[at + 1]) << 8) | (uint32_t(buffer[at + 2]) << 16) | (uint32_t(buffer[at + 3]) << 24); };
            const uint32_t imageAddress = word(0);
            const uint32_t bufferArgLen = std::min(word(4), kArgsMax);
            const ModuleLoadResult loaded = loadModuleFromIopMemory(imageAddress, bufferArgLen ? buffer.data() + kArgsOffset : nullptr, bufferArgLen);
            const int32_t id = loaded.moduleId > 0 ? loaded.moduleId : -1;
            const int32_t modres = loaded.moduleId > 0 ? loaded.startResult : -1;
            m_impl->host.log(LogLevel::Info, "[LOADFILE:HLE] load IOP buffer @" + std::to_string(imageAddress) + " -> id=" + std::to_string(id) +
                                                 " modres=" + std::to_string(modres));
            std::array<uint8_t, 8> reply{};
            for (unsigned i = 0; i < 4; ++i)
            {
                reply[i] = uint8_t(uint32_t(id) >> (8 * i));
                reply[4 + i] = uint8_t(uint32_t(modres) >> (8 * i));
            }
            if (!m_impl->host.writeGuest(request.receive.address, reply.data(), reply.size()))
                return result;
            result.resultAddress = request.receive.address;
            result.packetCompleted = true;
            return result;
        }
        if (request.function != 0u || request.send.size != kArgSize || request.receive.size != 8u ||
            request.send.address == 0u || request.receive.address == 0u)
        {
            m_impl->host.log(LogLevel::Warning, "[LOADFILE:HLE] unsupported call function=" + std::to_string(request.function) +
                                                    " send=" + std::to_string(request.send.size) +
                                                    " receive=" + std::to_string(request.receive.size));
            return result;
        }
        std::array<uint8_t, kArgSize> arg{};
        if (!m_impl->host.readGuest(request.send.address, arg.data(), arg.size()))
            return result;
        const uint32_t requestedArgLen = uint32_t(arg[0]) | (uint32_t(arg[1]) << 8) | (uint32_t(arg[2]) << 16) | (uint32_t(arg[3]) << 24);
        const uint32_t argLen = requestedArgLen > kArgsMax ? kArgsMax : requestedArgLen;
        size_t pathLen = 0u;
        while (pathLen < kPathMax && arg[kPathOffset + pathLen] != 0u)
            ++pathLen;
        const std::string path(reinterpret_cast<const char *>(arg.data() + kPathOffset), pathLen);
        const ModuleLoadResult loaded = loadModule(path, argLen ? arg.data() + kArgsOffset : nullptr, argLen);
        const int32_t id = loaded.moduleId > 0 ? loaded.moduleId : -1;
        const int32_t modres = loaded.moduleId > 0 ? loaded.startResult : -1;
        m_impl->host.log(LogLevel::Info, "[LOADFILE:HLE] load '" + path + "' -> id=" + std::to_string(id) +
                                             " modres=" + std::to_string(modres));
        std::array<uint8_t, 8> reply{};
        for (unsigned i = 0; i < 4; ++i)
        {
            reply[i] = uint8_t(uint32_t(id) >> (8 * i));
            reply[4 + i] = uint8_t(uint32_t(modres) >> (8 * i));
        }
        if (!m_impl->host.writeGuest(request.receive.address, reply.data(), reply.size()))
            return result;
        result.resultAddress = request.receive.address;
        result.packetCompleted = true;
        return result;
    }

    RpcResult IopSubsystem::handleRpc(const RpcRequest &request)
    {
        if (request.sid == kLoadFileSid)
        {
            return handleLoadFileRpc(request);
        }
        if (request.sid == detail::kFileIoSid)
        {
            return m_impl->fileio.handle(request);
        }
        const auto route = m_impl->routes.find(request.sid);
        detail::IopService *hle = route != m_impl->routes.end() ? route->second : nullptr;

        if (const char *trace = std::getenv("PS2X_RPC_TRACE"); trace && *trace)
        {
            static uint32_t traced = 0u;
            if (traced++ < 6000u)
            {
                std::ostringstream o;
                o << "[RPC:trace] sid=0x" << std::hex << request.sid << std::dec << " fn=" << request.function
                  << " send=" << request.send.size << " recv=" << request.receive.size
                  << (hle && hle->preferOverEmulatedServer() ? " via=HLE" : " via=default");
                if (request.send.size != 0u)
                {
                    uint32_t words[8] = {};
                    const uint32_t count = std::min<uint32_t>(request.send.size, sizeof(words));
                    if (m_impl->host.readGuest(request.send.address, words, count))
                    {
                        o << " send:" << std::hex;
                        for (uint32_t i = 0; i < (count + 3u) / 4u; ++i)
                            o << " " << words[i];
                    }
                }
                m_impl->host.log(LogLevel::Info, o.str());
            }
        }
        if (hle && hle->preferOverEmulatedServer())
        {
            return hle->handleRpc(request);
        }

        RpcResult emulated = m_impl->emulator.handleRpc(request);
        if (emulated.handled || !hle)
        {
            if (!emulated.packetCompleted)
            {
                std::string head;
                std::array<uint8_t, 48> raw{};
                const size_t count = std::min<size_t>(raw.size(), request.send.size);
                if (count && request.send.address && m_impl->host.readGuest(request.send.address, raw.data(), count))
                {
                    static const char hex[] = "0123456789abcdef";
                    for (size_t i = 0; i < count; ++i) { head += hex[raw[i] >> 4]; head += hex[raw[i] & 15]; }
                }
                m_impl->host.log(LogLevel::Warning, "[RPC] sid=0x" + [&] { std::ostringstream o; o << std::hex << request.sid; return o.str(); }() + " function=" + std::to_string(request.function) +
                                                         " send=" + std::to_string(request.send.size) + " receive=" + std::to_string(request.receive.size) + " head=" + head +
                                                         (emulated.handled ? " not completed by the emulator" : " has no server"));
            }
            return emulated;
        }
        return hle->handleRpc(request);
    }

    void IopSubsystem::onSifTransfer(const SifTransfer &transfer)
    {
        for (auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                service->onSifTransfer(transfer);
            }
        }
        m_impl->emulator.onSifTransfer(transfer);
        m_impl->packetAdapter.receive(*this, transfer);

        sif::ResetPacket resetRequest{};
        if (m_impl->packetAdapter.takeResetRequest(resetRequest))
        {
            // SifIopReset: the IOP restarts from scratch. Services, modules, RPC servers and
            // queued traffic of the previous session are gone; the EE sees the new IOP only
            // once the boot flags come up, after it has cleared them itself.
            // The UDNL argument (the IOPRP image) is not executed: services are provided by
            // the HLE/emulated modules, which the game loads again after the reboot.
            m_impl->host.log(LogLevel::Info, "[SIF:wire] IOP reboot requested arg='" + resetRequest.argument() + "'");
            reset();
            m_impl->bootPending = true;
            m_impl->bootCyclesLeft = sif::RebootLatencyEeCycles;
        }
    }

    uint32_t IopSubsystem::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->emulator.allocateMemory(size, alignment);
    }

    bool IopSubsystem::freeMemory(uint32_t address)
    {
        return m_impl->emulator.freeMemory(address);
    }

    bool IopSubsystem::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return m_impl->emulator.readMemory(address, destination, size);
    }

    bool IopSubsystem::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return m_impl->emulator.writeMemory(address, source, size);
    }

    bool IopSubsystem::zeroMemory(uint32_t address, size_t size)
    {
        return m_impl->emulator.zeroMemory(address, size);
    }

    bool IopSubsystem::isMemoryRange(uint32_t address, size_t size) const
    {
        return m_impl->emulator.isMemoryRange(address, size);
    }

    DebugSnapshot IopSubsystem::debugSnapshot() const
    {
        DebugSnapshot snapshot;
        snapshot.emulatorCycles = m_impl->emulator.cycles();
        snapshot.emulatorInstructions = m_impl->emulator.instructions();
        snapshot.emulatorLoadedModules = m_impl->emulator.loadedModuleCount();
        snapshot.emulatorThreads = m_impl->emulator.threadCount();
        snapshot.emulatorRpcServers = m_impl->emulator.rpcServerCount();
        snapshot.diagnostics = m_impl->loadOutcomes;
        if (!m_impl->lastError.empty())
        {
            snapshot.diagnostics.push_back(m_impl->lastError);
        }

        for (const auto &service : m_impl->coreServices)
        {
            DebugService row;
            row.name = service->name();
            row.sids.assign(service->sids().begin(), service->sids().end());
            row.active = m_impl->serviceActive(*service);
            service->appendDebugMetrics(row.metrics);
            snapshot.services.push_back(std::move(row));
        }
        return snapshot;
    }
}
