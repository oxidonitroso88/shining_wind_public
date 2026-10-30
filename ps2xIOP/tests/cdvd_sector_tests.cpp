#include "iop_compat_test_support.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/core/iop_memory.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_cpu.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <map>
using namespace ps2x::iop;
namespace {
constexpr size_t sector = 2048;
void both(uint8_t *p, uint32_t v, size_t n) {
    for(size_t i=0;i<n;++i) {p[i]=uint8_t(v>>(8*i));p[n+i]=uint8_t(v>>(8*(n-1-i)));}
}
void record(uint8_t *p, uint32_t lsn, uint32_t size, const std::string &name, bool dir) {
    p[0]=uint8_t((33+name.size()+1)&~size_t(1));both(p+2,lsn,4);both(p+10,size,4);
    p[25]=dir?2:0;both(p+28,1,2);p[32]=uint8_t(name.size());std::memcpy(p+33,name.data(),name.size());
}
std::vector<uint8_t> iso() {
    std::vector<uint8_t> b(40*sector);
    auto *p=b.data()+16*sector;p[0]=1;std::memcpy(p+1,"CD001",5);p[6]=1;
    both(p+80,40,4);both(p+120,1,2);both(p+124,1,2);both(p+128,sector,2);
    record(p+156,20,sector,std::string(1,'\0'),true);
    p=b.data()+17*sector;p[0]=255;std::memcpy(p+1,"CD001",5);p[6]=1;
    record(b.data()+20*sector,30,sector,"FILE.BIN;1",false);
    std::fill(b.begin()+30*sector,b.begin()+31*sector,0xA5);
    return b;
}
struct Host : iop_test::Host {
    std::string root,imagePath="disc.iso";
    std::vector<uint8_t> image=iso();
    bool shortRead=false;
    std::map<uint64_t,std::string> files;uint64_t serial=10;
    std::string hostPath(HostPathKind k) const override {return k==HostPathKind::CdImage?imagePath:k==HostPathKind::CdRoot?root:std::string{};}
    uint64_t openHostFile(std::string_view path) override {
        if(path=="disc.iso")return image.empty()?0:1;
        if(!std::filesystem::is_regular_file(path))return 0;
        files[++serial]=std::string(path);return serial;
    }
    bool hostFileSize(uint64_t h,uint64_t &size) const override {
        if(h==1){size=image.size();return true;}
        auto it=files.find(h);if(it==files.end())return false;
        size=std::filesystem::file_size(it->second);return true;
    }
    bool readHostFile(uint64_t h,uint64_t offset,void *dst,size_t size,size_t &got) override {
        got=0;if(h==1){if(offset>image.size())return false;got=std::min(size,image.size()-size_t(offset));if(shortRead && got) --got;std::memcpy(dst,image.data()+offset,got);return true;}
        auto it=files.find(h);if(it==files.end())return false;
        std::ifstream f(it->second,std::ios::binary);f.seekg(offset);f.read(static_cast<char*>(dst),size);got=size_t(f.gcount());return true;
    }
    void closeHostFile(uint64_t h) override {files.erase(h);}
};
}
int main() {
    const auto directory=std::filesystem::temp_directory_path()/("cdvd-sector-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(directory);
        {std::ofstream f(directory/"FILE.BIN",std::ios::binary);std::string data(sector,char(0x5A));f.write(data.data(),data.size());}
        Host host;host.root=directory.string();detail::IopMemory memory;detail::IopKernel kernel(memory);detail::IopCdvd cd(host,memory,kernel);cd.reset();
        memory.zeroRam(0x4000,2*sector); // CDVD requires an owned IOP destination.
        size_t checks=0;auto check=[&](bool v,const char *s){iop_test::require(v,s);++checks;};
        auto call=[&](uint16_t ordinal,uint32_t a,uint32_t b,uint32_t c=0){detail::IopCpuState cpu{};cpu.gpr[4]=a;cpu.gpr[5]=b;cpu.gpr[6]=c;check(cd.dispatchImport(ordinal,cpu),"import unhandled");return cpu.gpr[2];};
        const std::string path="cdrom0:/FILE.BIN;1";memory.writeRam(0x1000,path.c_str(),path.size()+1);
        auto search=[&](){return call(10,0x1100,0x1000);};
        auto lsn=[&](){uint32_t n=0;memory.readRam(0x1100,&n,4);return n;};
        auto read=[&](uint32_t n,uint32_t count=1){return call(6,n,count,0x4000);};
        auto payload=[&](uint8_t value){std::array<uint8_t,sector> b{};memory.readRam(0x4000,b.data(),b.size());return std::all_of(b.begin(),b.end(),[&](uint8_t x){return x==value;});};
        check(search()==1,"image search failed");const auto imageLsn=lsn();
        const auto firstRead = read(imageLsn);
        std::cout << "image search LSN=" << imageLsn << " read=" << firstRead << " payload=" << payload(0xA5) << '\n';
        check(firstRead==1 && payload(0xA5),"Search->Read mixed virtual LSN with image bytes");
        check(imageLsn==30,"wrong ISO extent");
        host.shortRead=true;check(read(30)==0 && payload(0xA5),"short image read published partial data");host.shortRead=false;
        const std::string missing="cdrom0:/MISSING.BIN;1";memory.writeRam(0x1000,missing.c_str(),missing.size()+1);
        check(search()==0 && lsn()==30,"missing search modified output or fabricated result");
        memory.writeRam(0x1000,path.c_str(),path.size()+1);
        check(read(40)==0 && read(39,2)==0 && read(0xFFFFFFFFu,2)==0,"out of volume read succeeded");
        check(call(6,30,1,0x1FFFFF)==0,"invalid destination accepted");
        host.imagePath.clear();check(search()==1 && lsn()==30 && read(30)==1 && payload(0xA5),"configuration changed active image without reset");
        cd.reset();check(search()==1,"directory search failed");const auto virtualLsn=lsn();
        check(virtualLsn!=30 && read(virtualLsn)==1 && payload(0x5A),"virtual Search->Read mismatch");
        host.imagePath="disc.iso";check(read(virtualLsn)==1 && payload(0x5A),"directory sectors reinterpreted as image");
        check(read(0xFFFFFFFFu)==0,"virtual bounds not enforced");
        cd.reset();check(search()==1 && lsn()==30,"reset did not select new image generation");
        host.image.resize(30*sector);check(read(30)==0,"truncated image fell back to directory");
        for(int variant=0;variant<11;++variant){
            host.image=iso();
            if(variant==0)host.image.clear();
            if(variant==1)host.image[16*sector+1]='X';
            if(variant==2)host.image.resize(17*sector);
            if(variant==3)both(host.image.data()+16*sector+128,1024,2);
            if(variant==4)host.image[20*sector]=10;
            if(variant==5)both(host.image.data()+20*sector+2,40,4);
            if(variant==6)host.image[20*sector+25]=128;
            if(variant==7)host.image[20*sector+6]^=1;
            if(variant==8)host.image[20*sector+32]=250;
            if(variant==9)host.image[20*sector+26]=1;
            if(variant==10)host.image[16*sector+6]=2;
            cd.reset();check(search()==0,"invalid image metadata fell back or was accepted");
            if(variant<4)check(read(virtualLsn)==0,"invalid image used directory sectors");
        }
        host.image=iso();host.root.clear();cd.reset();check(search()==1 && read(lsn())==1 && payload(0xA5),"image-only source failed");
        // Traverse a subdirectory whose first sector is padding; data record in the next sector.
        host.image=iso();std::fill(host.image.begin()+20*sector,host.image.begin()+21*sector,0);
        record(host.image.data()+20*sector,22,2*sector,"DIR",true);
        record(host.image.data()+23*sector,30,sector,"FILE.BIN;1",false);
        const std::string nested="cdrom0:/DIR/FILE.BIN;1";memory.writeRam(0x1000,nested.c_str(),nested.size()+1);
        cd.reset();check(search()==1 && lsn()==30 && read(lsn())==1 && payload(0xA5),"nested sector padding failed");
        memory.writeRam(0x1000,path.c_str(),path.size()+1);
        host.imagePath="missing.iso";cd.reset();check(search()==0 && read(virtualLsn)==0,"missing configured image fell back");
        host.imagePath.clear();host.root.clear();cd.reset();check(search()==0 && read(30)==0,"unconfigured source succeeded");
        check(cd.deviceSnapshot().activeOperations==0,"failed read leaked device work");
        std::filesystem::remove(directory/"FILE.BIN");std::filesystem::remove(directory);
        std::cout<<"PASS "<<checks<<" CDVD sector checks\n";return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
