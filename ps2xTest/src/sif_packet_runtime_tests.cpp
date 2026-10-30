#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_iop_transport.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "ps2x/iop/sif_packet.h"
#include <cstring>
#include <vector>

namespace ps2_stubs { void resetSifState(); }
namespace
{
    using namespace ps2x::iop::sif;
    // Synthetic SDK, all addresses belong to the test, not a game.
    constexpr uint32_t Entry=0x100000, Resume=0x100010, Dispatcher=0x100020;
    constexpr uint32_t Handler=0x100030, EndHandler=0x100040, HostHandler=0x100050;
    constexpr uint32_t Buffer=0x2000, Sreg=0x2100, Table=0x2200, PacketAddress=0x2400;
    constexpr uint32_t Descriptor=0x2500, Client=0x2600, RpcPacket=0x2700, Reply=0x2800;
    uint32_t phase, stage, dispatches, hostCalls;
    bool failed, resetBeforeDispatch;
    bool maskAtInit, maskResumed;
    bool cdvdRun, rebootRun;
    uint32_t flagsAfterClears, flagsEarly, flagsLate;
    uint32_t cdvdStep;
    constexpr uint32_t CdvdSids[] = {0x80000592, 0x80000593, 0x8000059c};
    std::vector<uint32_t> commands;
    uint32_t read(uint8_t *ram, uint32_t address) { uint32_t v; std::memcpy(&v,ram+address,4); return v; }
    void write(uint8_t *ram, uint32_t address, uint32_t value) { std::memcpy(ram+address,&value,4); }
    Packet make(uint32_t command,uint32_t size) { Packet p{};p.size=size;p.words[0]=size;p.words[2]=command;return p; }
    void send(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime,const Packet &p)
    {
        const auto bytes=encode(p);
        std::memcpy(ram+PacketAddress,bytes.data(),p.size);
        const uint32_t desc[]={PacketAddress,0x400,p.size,0x44};
        std::memcpy(ram+Descriptor,desc,sizeof(desc));
        SET_GPR_U32(ctx,4,Descriptor); SET_GPR_U32(ctx,5,1);
        ctx->pc=Resume;
        ps2_stubs::sceSifSetDma(ram,ctx,runtime);
        failed |= getRegU32(ctx,2)==0;
    }
    void dispatcher(uint8_t *ram,R5900Context *ctx,PS2Runtime *)
    {
        ++dispatches;
        const uint32_t command=read(ram,Buffer+8);
        commands.push_back(command);
        failed |= ram[Buffer]==0;
        ram[Buffer]=0; // SDK receiver consumes the transport slot.
        const uint32_t index=command&0x7fffffff;
        ctx->pc=index<16 ? read(ram,Table+index*8) : 0;
        SET_GPR_U32(ctx,4,Buffer); SET_GPR_U32(ctx,5,read(ram,Table+index*8+4));
    }
    void handler(uint8_t *ram,R5900Context *ctx,PS2Runtime *)
    {
        const auto packet=getRegU32(ctx,4), sregs=getRegU32(ctx,5);
        write(ram,sregs+read(ram,packet+16)*4,read(ram,packet+20));
        stage=1; ctx->pc=0;
    }
    void endHandler(uint8_t *ram,R5900Context *ctx,PS2Runtime *)
    {
        const auto packet=getRegU32(ctx,4);
        failed |= read(ram,packet+28)!=Client;
        if(read(ram,packet+32)==Bind)
        {
            write(ram,Client+36,read(ram,packet+36));
            write(ram,Client+20,read(ram,packet+40)); stage=2;
        }
        else if(read(ram,packet+32)==Call) stage=3;
        else failed=true;
        ctx->pc=0;
    }
    void hostHandler(uint8_t *,R5900Context *ctx,PS2Runtime *) { ++hostCalls;ctx->pc=0; }
    void entry(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
    {
        runtime->eeScheduler().addIrqHandler(true,5,Dispatcher,true,0,0,0);
        if (maskAtInit) runtime->eeScheduler().setIrqCauseEnabled(true,5,false);
        write(ram,Table+8,Handler);write(ram,Table+12,Sreg);
        write(ram,Table+8*8,EndHandler);
        SET_GPR_U32(ctx,4,SetSreg);SET_GPR_U32(ctx,5,HostHandler);SET_GPR_U32(ctx,6,0);
        ps2_stubs::sceSifAddCmdHandler(ram,ctx,runtime);
        auto p=make(Init,20);p.words[4]=Buffer;
        send(ram,ctx,runtime,p);
        failed |= read(ram,Sreg)!=0 || dispatches!=0;
        if(resetBeforeDispatch) PS2IopTransport::reset(runtime);
    }
    void resume(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime)
    {
        if (maskAtInit && !maskResumed)
        {
            failed |= dispatches!=0 || read(ram,Sreg)!=0;
            maskResumed=true;
            runtime->eeScheduler().setIrqCauseEnabled(true,5,true);
            auto sync=make(Init,16);sync.words[3]=1;send(ram,ctx,runtime,sync);
            return;
        }
        if(rebootRun)
        {
            auto reg=[&](uint32_t r,uint32_t v){ SET_GPR_U32(ctx,4,r);SET_GPR_U32(ctx,5,v);ps2_stubs::sceSifSetReg(ram,ctx,runtime); };
            auto get=[&](uint32_t r){ SET_GPR_U32(ctx,4,r);ps2_stubs::sceSifGetReg(ram,ctx,runtime);return getRegU32(ctx,2); };
            if(phase==0)
            {
                failed |= stage!=1 || read(ram,Sreg)!=1;
                ++phase;
                // PS2SDK SifIopReset order: clear BOOTEND, send SIFCMD reset, clear SIFINIT and CMDINIT.
                reg(4,0x40000);
                uint8_t bytes[104]={}; bytes[0]=104; const uint32_t cid=Reset; std::memcpy(bytes+8,&cid,4);
                const char arg[]="rom0:UDNL cdrom0:/MODULES/IOPRP300.IMG;1"; const uint32_t len=sizeof(arg)-1; std::memcpy(bytes+16,&len,4);
                std::memcpy(bytes+24,arg,len);
                std::memcpy(ram+PacketAddress,bytes,sizeof(bytes));
                const uint32_t desc[]={PacketAddress,0x400,104,0x44};
                std::memcpy(ram+Descriptor,desc,sizeof(desc));
                SET_GPR_U32(ctx,4,Descriptor);SET_GPR_U32(ctx,5,1);
                ps2_stubs::sceSifSetDma(ram,ctx,runtime);
                failed |= getRegU32(ctx,2)==0;
                reg(4,0x10000); reg(4,0x20000); reg(0x80000002,0); reg(0x80000000,0);
                flagsAfterClears=get(4);
                PS2IopTransport::advance(runtime,1000);
                flagsEarly=get(4);
                PS2IopTransport::advance(runtime,RebootLatencyEeCycles);
                flagsLate=get(4);
                // The rebooted IOP has no SIFCMD endpoint until the SDK sends INIT_CMD again.
                auto p=make(Init,20);p.words[4]=Buffer;
                send(ram,ctx,runtime,p);
                return;
            }
            failed |= dispatches!=2 || read(ram,Sreg)!=1;
            runtime->requestStop();ctx->pc=0;return;
        }
        if(resetBeforeDispatch) { failed |= dispatches!=0 || read(ram,Sreg)!=0; runtime->requestStop();ctx->pc=0;return; }
        if(phase==0)
        {
            failed |= stage!=1 || read(ram,Sreg)!=1;
            ++phase;
            auto sync=make(Init,16);sync.words[3]=1;send(ram,ctx,runtime,sync);
            auto bind=make(Bind,64);bind.words[4]=6;bind.words[5]=RpcPacket;
            bind.words[6]=1;bind.words[7]=Client;bind.words[8]=cdvdRun ? CdvdSids[cdvdStep] : 0x80001300;
            send(ram,ctx,runtime,bind);
        }
        else if(phase==1)
        {
            failed |= stage!=2 || !read(ram,Client+36);
            ++phase;
            auto call=make(Call,64);call.words[4]=6;call.words[5]=RpcPacket;call.words[6]=2;
            call.words[7]=Client;call.words[8]=0x80001363;call.words[10]=Reply;call.words[11]=16;
            call.words[12]=1;call.words[13]=read(ram,Client+36);
            if(cdvdRun)
            {
                // SDK data DMA precedes the command packet and uses the server
                // buffer returned by the guest BIND END handler.
                write(ram,Reply+32,cdvdStep==1 ? 2 : 0);
                const uint32_t desc[]={Reply+32,read(ram,Client+20),4,0};
                std::memcpy(ram+Descriptor,desc,sizeof(desc));
                SET_GPR_U32(ctx,4,Descriptor);SET_GPR_U32(ctx,5,1);
                ps2_stubs::sceSifSetDma(ram,ctx,runtime);
                failed |= getRegU32(ctx,2)==0;
                call.words[0]|=4<<8;call.words[1]=read(ram,Client+20);
                call.words[8]=cdvdStep==1 ? 0x22 : 0;call.words[9]=4;
                call.words[11]=cdvdStep==0 ? 16 : 4;
            }
            send(ram,ctx,runtime,call);
        }
        else
        {
            failed |= stage!=3 || read(ram,Reply)!=(cdvdRun ? (cdvdStep==2 ? 2u : 1u) : 0x310u);
            if(cdvdRun && ++cdvdStep<3)
            {
                phase=0;stage=1;ctx->pc=Resume;return;
            }
            runtime->requestStop();ctx->pc=0;
        }
    }
    void scenario(TestCase &t,bool reset,bool mask=false,bool cdvd=false,bool reboot=false)
    {
        PS2Runtime runtime;
        ps2_stubs::resetSifState();
        t.IsTrue(runtime.memory().initialize(),"synthetic EE memory initializes");
        auto *ram=runtime.memory().getRDRAM();
        if (!ram) return;
        std::memset(ram+Buffer,0,0x1000);
        phase=stage=dispatches=hostCalls=0;failed=false;resetBeforeDispatch=reset;commands.clear();
        maskAtInit=mask;maskResumed=false;
        cdvdRun=cdvd;cdvdStep=0;rebootRun=reboot;flagsAfterClears=flagsEarly=flagsLate=0xdeadbeef;
        runtime.registerFunction(Entry,entry);runtime.registerFunction(Resume,resume);
        runtime.registerFunction(Dispatcher,dispatcher);runtime.registerFunction(Handler,handler);
        runtime.registerFunction(EndHandler,endHandler);runtime.registerFunction(HostHandler,hostHandler);
        if(!cdvd) t.IsTrue(runtime.loadIopModule("rom0:DBCMAN").moduleId>0,"existing HLE backend loaded");
        R5900Context ctx{};ctx.pc=Entry;
        runtime.eeScheduler().reset(ram,ctx);runtime.eeScheduler().run();
        t.IsTrue(!failed,"guest handshake and RPC obey event order and payload");
        t.Equals(hostCalls,0u,"host command map must not intercept guest SDK packets");
        if(reboot)
        {
            t.Equals(flagsAfterClears,0u,"SDK clears of SMFLAG (write-1-to-clear) leave no boot flag set");
            t.Equals(flagsEarly,0u,"rebooted IOP does not raise BOOTEND before its boot latency");
            t.Equals(flagsLate,0x70000u,"rebooted IOP raises SIFINIT|CMDINIT|BOOTEND after the latency");
            t.Equals(dispatches,2u,"SIFCMD handshake is repeated after the reboot");
            return;
        }
        if(!reset)
        {
            t.Equals(dispatches,cdvd ? 7u : 3u,"guest dispatcher runs for SET_SREG, BIND END and CALL END");
            const auto expected=cdvd ? std::vector<uint32_t>{SetSreg,End,End,End,End,End,End} :
                                     std::vector<uint32_t>{SetSreg,End,End};
            t.IsTrue(commands==expected,"command IDs preserved in order");
        }
    }
}
void register_sif_packet_runtime_tests()
{
    MiniTest::Case("SifPacketRuntime",[](TestCase &tc) {
        tc.Run("guest INIT and RPC through transport backend and guest dispatcher",[](TestCase &t){ scenario(t,false); });
        tc.Run("reset cancels queued guest delivery without writing SREG",[](TestCase &t){ scenario(t,true); });
        tc.Run("masked SIF0 retains packets until guest interrupt enabled",[](TestCase &t){ scenario(t,false,true); });
        tc.Run("SIFCMD reset (104 bytes) reboots the IOP and raises SMFLAG after the SDK clears it",[](TestCase &t){ scenario(t,false,false,false,true); });
        tc.Run("CDVDFSV INIT Mmode Ready through guest dispatcher",[](TestCase &t){ scenario(t,false,false,true); });
    });
}
