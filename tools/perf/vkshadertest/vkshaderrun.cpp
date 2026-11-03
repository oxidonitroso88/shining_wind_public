// vkshaderrun: creates the COMPUTE PIPELINE of parallel-gs' sampler_feedback.comp from a SPIR-V file, runs it on deterministic synthetic input and prints a canonical
// digest of what it wrote, so that two SPIR-V builds of the same shader (original vs patched) or two GPUs can be compared bit for bit.
//
//   vkshaderrun [--device N] [--prims N] [--seed S] file.spv [file.spv ...]
//
// Resource contract (read from the shader's SPIR-V, see docs/vulkan-amd-startup-crash-2026-10-30.md): push constants {num_primitives, num_textures, super_sample_offset};
// set 0: binding 4 SSBO PrimitiveSetup[80 B], 7 SSBO TransformedAttributes[64 B], 8 SSBO PrimitiveAttribute[32 B], 9 UBO StateVector[4096 x 16 B], 15 UBO TexInfo[1024 x 48 B];
// set 1: binding 0 UBO TextureAnalysis[1024 x 48 B] = { u64 dispatch_va, u64 workgroups_va, u64 bitmask_va, uvec2 size_minus_1, u16vec2 base, uint block_stride_layers, uint flags }.
// The three addresses point to per-texture buffers (VK_KHR_buffer_device_address) that the shader fills with atomics: indirect dispatch {x,y,z}, {valid, workgroup list}, bitmask.
// Output per texture = dispatch[3], valid, bitmask words, and the SORTED workgroup list (the atomicAdd order is not deterministic), hashed with FNV-1a.
//
// Each stage reports separately (module / pipeline / dispatch) and an access violation inside the driver is caught, so the tool also documents WHERE a driver fails.
#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static PFN_vkGetInstanceProcAddr gGIPA;
static PFN_vkGetDeviceProcAddr gGDPA;

#define DEV(name) static PFN_##name name##_;
DEV(vkCreateShaderModule) DEV(vkDestroyShaderModule) DEV(vkCreateDescriptorSetLayout) DEV(vkCreatePipelineLayout) DEV(vkCreateComputePipelines) DEV(vkCreateBuffer) DEV(vkGetBufferMemoryRequirements)
DEV(vkAllocateMemory) DEV(vkBindBufferMemory) DEV(vkMapMemory) DEV(vkCreateDescriptorPool) DEV(vkAllocateDescriptorSets) DEV(vkUpdateDescriptorSets) DEV(vkCreateCommandPool)
DEV(vkAllocateCommandBuffers) DEV(vkBeginCommandBuffer) DEV(vkEndCommandBuffer) DEV(vkCmdBindPipeline) DEV(vkCmdBindDescriptorSets) DEV(vkCmdPushConstants) DEV(vkCmdDispatch)
DEV(vkGetDeviceQueue) DEV(vkQueueSubmit) DEV(vkQueueWaitIdle) DEV(vkGetBufferDeviceAddress) DEV(vkDestroyPipeline) DEV(vkDestroyPipelineLayout) DEV(vkDestroyDescriptorSetLayout)
DEV(vkDestroyBuffer) DEV(vkFreeMemory) DEV(vkDestroyDescriptorPool) DEV(vkDestroyCommandPool) DEV(vkCmdPipelineBarrier)

static VkPhysicalDevice gPhys;
static VkDevice gDev;
static VkPhysicalDeviceMemoryProperties gMem;

struct Buf
{
    VkBuffer buffer = nullptr;
    VkDeviceMemory memory = nullptr;
    uint8_t *ptr = nullptr;
    VkDeviceSize size = 0;
    uint64_t address = 0;
};

static uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < gMem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (gMem.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return ~0u;
}

static Buf makeBuf(VkDeviceSize size, VkBufferUsageFlags usage, bool address)
{
    Buf b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage | (address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
    vkCreateBuffer_(gDev, &ci, nullptr, &b.buffer);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements_(gDev, b.buffer, &mr);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = address ? &flags : nullptr;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory_(gDev, &ai, nullptr, &b.memory);
    vkBindBufferMemory_(gDev, b.buffer, b.memory, 0);
    void *p = nullptr;
    vkMapMemory_(gDev, b.memory, 0, VK_WHOLE_SIZE, 0, &p);
    b.ptr = static_cast<uint8_t *>(p);
    memset(b.ptr, 0, static_cast<size_t>(size));
    if (address)
    {
        VkBufferDeviceAddressInfo ai2{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai2.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress_(gDev, &ai2);
    }
    return b;
}

static uint64_t gRng;
static uint32_t rnd()
{
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return static_cast<uint32_t>(gRng >> 11);
}
static float rndf(float lo, float hi) { return lo + (hi - lo) * (static_cast<float>(rnd() & 0xFFFFFF) / 16777216.0f); }

static std::vector<uint32_t> readSpv(const char *path)
{
    std::vector<uint32_t> v;
    FILE *f = fopen(path, "rb");
    if (!f)
        return v;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize((n + 3) / 4);
    if (fread(v.data(), 1, n, f) != static_cast<size_t>(n))
        v.clear();
    fclose(f);
    return v;
}

static int gFault = 0;
static void *gFaultAt = nullptr;
static int onFault(EXCEPTION_POINTERS *i)
{
    gFault = static_cast<int>(i->ExceptionRecord->ExceptionCode);
    gFaultAt = i->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
static std::string where(void *a)
{
    HMODULE m = nullptr;
    char buf[300] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(a), &m) && m)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(m, path, MAX_PATH);
        const char *b = strrchr(path, '\\');
        snprintf(buf, sizeof(buf), "%s+0x%llx", b ? b + 1 : path, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(a) - reinterpret_cast<uintptr_t>(m)));
    }
    return buf;
}
// No C++ objects with destructors: __try/__except.
static bool callCreateModule(const VkShaderModuleCreateInfo *ci, VkShaderModule *m, VkResult *r)
{
    __try { *r = vkCreateShaderModule_(gDev, ci, nullptr, m); return true; }
    __except (onFault(GetExceptionInformation())) { return false; }
}
static bool callCreatePipeline(const VkComputePipelineCreateInfo *ci, VkPipeline *p, VkResult *r)
{
    __try { *r = vkCreateComputePipelines_(gDev, nullptr, 1, ci, nullptr, p); return true; }
    __except (onFault(GetExceptionInformation())) { return false; }
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

constexpr uint32_t kTextures = 8;
constexpr uint32_t kMaxWorkgroups = 4096;
constexpr uint32_t kBitmaskWords = 128;

int main(int argc, char **argv)
{
    uint32_t deviceIndex = 0, prims = 20000;
    uint64_t seed = 12345;
    std::vector<const char *> files;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) deviceIndex = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prims") && i + 1 < argc) prims = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], nullptr, 10);
        else files.push_back(argv[i]);
    }
    HMODULE loader = LoadLibraryA("vulkan-1.dll");
    if (!loader) { printf("vulkan-1.dll not found\n"); return 2; }
    gGIPA = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gGIPA(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = nullptr;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return 2; }
#define INST(name) auto name = reinterpret_cast<PFN_##name>(gGIPA(inst, #name))
    INST(vkEnumeratePhysicalDevices); INST(vkGetPhysicalDeviceProperties); INST(vkGetPhysicalDeviceFeatures2); INST(vkGetPhysicalDeviceQueueFamilyProperties);
    INST(vkGetPhysicalDeviceMemoryProperties); INST(vkCreateDevice); INST(vkGetDeviceProcAddr);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    for (uint32_t i = 0; i < n; ++i)
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devs[i], &p);
        printf("device %u: %s (vendor 0x%04x, driver 0x%08x)%s\n", i, p.deviceName, p.vendorID, p.driverVersion, i == deviceIndex ? "  <- selected" : "");
    }
    if (deviceIndex >= n || files.empty()) return files.empty() ? 0 : 2;
    gPhys = devs[deviceIndex];
    vkGetPhysicalDeviceMemoryProperties(gPhys, &gMem);

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feats.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(gPhys, &feats);
    printf("features: shaderInt16=%d bufferDeviceAddress=%d storageBuffer16=%d uniformAndStorageBuffer16=%d\n", feats.features.shaderInt16, f12.bufferDeviceAddress, f11.storageBuffer16BitAccess,
           f11.uniformAndStorageBuffer16BitAccess);
    if (!f12.bufferDeviceAddress) { printf("bufferDeviceAddress not supported: cannot run\n"); return 2; }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gPhys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(gPhys, &nq, qf.data());
    uint32_t family = 0;
    for (uint32_t i = 0; i < nq; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &feats; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(gPhys, &dci, nullptr, &gDev) != VK_SUCCESS) { printf("vkCreateDevice failed\n"); return 2; }
    gGDPA = vkGetDeviceProcAddr;
#define LOAD(name) name##_ = reinterpret_cast<PFN_##name>(gGDPA(gDev, #name))
    LOAD(vkCreateShaderModule); LOAD(vkDestroyShaderModule); LOAD(vkCreateDescriptorSetLayout); LOAD(vkCreatePipelineLayout); LOAD(vkCreateComputePipelines); LOAD(vkCreateBuffer);
    LOAD(vkGetBufferMemoryRequirements); LOAD(vkAllocateMemory); LOAD(vkBindBufferMemory); LOAD(vkMapMemory); LOAD(vkCreateDescriptorPool); LOAD(vkAllocateDescriptorSets);
    LOAD(vkUpdateDescriptorSets); LOAD(vkCreateCommandPool); LOAD(vkAllocateCommandBuffers); LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer); LOAD(vkCmdBindPipeline);
    LOAD(vkCmdBindDescriptorSets); LOAD(vkCmdPushConstants); LOAD(vkCmdDispatch); LOAD(vkGetDeviceQueue); LOAD(vkQueueSubmit); LOAD(vkQueueWaitIdle); LOAD(vkGetBufferDeviceAddress);
    LOAD(vkDestroyPipeline); LOAD(vkDestroyPipelineLayout); LOAD(vkDestroyDescriptorSetLayout); LOAD(vkDestroyBuffer); LOAD(vkFreeMemory); LOAD(vkDestroyDescriptorPool);
    LOAD(vkDestroyCommandPool); LOAD(vkCmdPipelineBarrier);
    VkQueue queue;
    vkGetDeviceQueue_(gDev, family, 0, &queue);

    int failures = 0;
    for (const char *path : files)
    {
        printf("\n== %s\n", path);
        fflush(stdout);
        std::vector<uint32_t> code = readSpv(path);
        if (code.size() < 5 || code[0] != 0x07230203u) { printf("   not SPIR-V\n"); ++failures; continue; }

        // ---- layouts
        VkDescriptorSetLayoutBinding set0[5] = {};
        const uint32_t b0[5] = {4, 7, 8, 9, 15};
        const VkDescriptorType t0[5] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
        for (int i = 0; i < 5; ++i) { set0[i].binding = b0[i]; set0[i].descriptorType = t0[i]; set0[i].descriptorCount = 1; set0[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
        VkDescriptorSetLayoutBinding set1[1] = {};
        set1[0].binding = 0; set1[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; set1[0].descriptorCount = 1; set1[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dl0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}, dl1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl0.bindingCount = 5; dl0.pBindings = set0; dl1.bindingCount = 1; dl1.pBindings = set1;
        VkDescriptorSetLayout sl[2];
        vkCreateDescriptorSetLayout_(gDev, &dl0, nullptr, &sl[0]);
        vkCreateDescriptorSetLayout_(gDev, &dl1, nullptr, &sl[1]);
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 2; plci.pSetLayouts = sl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        VkPipelineLayout pl;
        vkCreatePipelineLayout_(gDev, &plci, nullptr, &pl);

        // ---- module + pipeline (each stage reports separately)
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = code.size() * 4; smci.pCode = code.data();
        VkShaderModule mod = nullptr;
        VkResult r = VK_SUCCESS;
        printf("   vkCreateShaderModule ... "); fflush(stdout);
        if (!callCreateModule(&smci, &mod, &r)) { printf("CRASH 0x%08X at %s\n", gFault, where(gFaultAt).c_str()); ++failures; continue; }
        printf("%s\n", r == VK_SUCCESS ? "OK" : "FAILED"); if (r != VK_SUCCESS) { ++failures; continue; }
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = mod; cpci.stage.pName = "main";
        cpci.layout = pl;
        VkPipeline pipe = nullptr;
        printf("   vkCreateComputePipelines ... "); fflush(stdout);
        if (!callCreatePipeline(&cpci, &pipe, &r)) { printf("CRASH 0x%08X at %s\n", gFault, where(gFaultAt).c_str()); ++failures; continue; }
        printf("%s (VkResult %d)\n", r == VK_SUCCESS ? "OK" : "FAILED", static_cast<int>(r)); if (r != VK_SUCCESS) { ++failures; continue; }

        // ---- synthetic input (same for every file: the generator is re-seeded)
        gRng = seed * 0x9E3779B97F4A7C15ull + 1;
        const VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ubo = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        Buf setupB = makeBuf(80ull * prims, ssbo, false), attrXB = makeBuf(64ull * prims, ssbo, false), attrB = makeBuf(32ull * prims, ssbo, false);
        Buf stateB = makeBuf(16ull * 4096, ubo, false), texinfoB = makeBuf(48ull * 1024, ubo, false), analysisB = makeBuf(48ull * 1024, ubo, false);
        Buf disp[kTextures], wg[kTextures], mask[kTextures];
        uint32_t sizeM1[kTextures];
        for (uint32_t t = 0; t < kTextures; ++t)
        {
            disp[t] = makeBuf(16, ssbo, true);
            wg[t] = makeBuf(16 + 8ull * kMaxWorkgroups, ssbo, true);
            mask[t] = makeBuf(4ull * kBitmaskWords, ssbo, true);
            const uint32_t size = 64 + 32 * (t % 5); // 64..192: includes non power-of-two sizes
            sizeM1[t] = size - 1;
            const uint32_t blocksX = (sizeM1[t] >> 3) + 1, layers = 1 + (t & 1);
            uint8_t *a = analysisB.ptr + 48ull * t;
            uint64_t va0 = disp[t].address, va1 = wg[t].address, va2 = mask[t].address;
            memcpy(a + 0, &va0, 8); memcpy(a + 8, &va1, 8); memcpy(a + 16, &va2, 8);
            uint32_t sm[2] = {sizeM1[t], sizeM1[t]};
            memcpy(a + 24, sm, 8);
            uint16_t base[2] = {static_cast<uint16_t>(rnd() % 16), static_cast<uint16_t>(rnd() % 16)};
            memcpy(a + 32, base, 4);
            uint32_t bsl = blocksX | (layers << 16), flags = (t == 7) ? 0u : 1u; // texture 7 disabled
            memcpy(a + 36, &bsl, 4); memcpy(a + 40, &flags, 4);
            float sizes[4] = {static_cast<float>(size), static_cast<float>(size), 1.0f / size, 1.0f / size};
            memcpy(texinfoB.ptr + 48ull * t, sizes, 16);
        }
        for (uint32_t s = 0; s < 64; ++s)
        {
            uint32_t comb = (rnd() % 10) ? (1u << 2) : 0u; // COMBINER_TME_BIT in 90% of the states
            memcpy(stateB.ptr + 16ull * s, &comb, 4);
        }
        uint32_t wide = 0;
        for (uint32_t i = 0; i < prims; ++i)
        {
            const bool isWide = (rnd() % 8) == 0;
            wide += isWide;
            const uint32_t stateIdx = rnd() % 64, tex = (rnd() % 20 == 0) ? kTextures + (rnd() % 3) : rnd() % kTextures; // some indices >= num_textures
            const bool persp = rnd() & 1, snap = rnd() & 1;
            uint32_t state = stateIdx | (persp ? (1u << 18) : 0u) | (snap ? (1u << 26) : 0u);
            uint32_t texword = tex | ((rnd() & 1) ? (1u << 16) : 0u) | ((rnd() & 1) ? (1u << 19) : 0u) | ((rnd() & 1) ? (1u << 20) : 0u);
            int16_t x0 = static_cast<int16_t>(rnd() % 48), y0 = static_cast<int16_t>(rnd() % 48), w = static_cast<int16_t>(rnd() % 16);
            int16_t bb[4] = {x0, y0, static_cast<int16_t>(x0 + w), static_cast<int16_t>(y0 + (rnd() % 16))};
            if (rnd() % 40 == 0) std::swap(bb[0], bb[2]); // empty
            uint8_t *pa = attrB.ptr + 32ull * i;
            memcpy(pa, bb, 8); memcpy(pa + 8, &state, 4); memcpy(pa + 12, &texword, 4);
            uint8_t *ps = setupB.ptr + 80ull * i;
            const int span = isWide ? 40 : 3;
            int32_t bv[3] = {static_cast<int32_t>(rnd() % (2 * span + 1)) - span, static_cast<int32_t>(rnd() % (2 * span + 1)) - span, 0};
            int32_t cv[3] = {static_cast<int32_t>(rnd() % (2 * span + 1)) - span, static_cast<int32_t>(rnd() % (2 * span + 1)) - span, 0};
            float inv_area = 1.0f / (2048.0f + (rnd() % 2048)), ei = rndf(-0.2f, 0.8f), ej = rndf(-0.2f, 0.8f);
            memcpy(ps + 16, bv, 12); memcpy(ps + 28, &ei, 4); memcpy(ps + 32, cv, 12); memcpy(ps + 44, &ej, 4);
            memcpy(ps + 12, &inv_area, 4);
            const float q = rndf(0.5f, 2.0f), u0 = rndf(-0.1f, 1.1f), v0 = rndf(-0.1f, 1.1f), du = rndf(-0.4f, 0.4f), dv = rndf(-0.4f, 0.4f), size = 64.0f + 32.0f * (tex % 5);
            float stqf[12];
            if (persp) { float a0[4] = {u0 * q, v0 * q, q, 0}, a1[4] = {du * q, dv * q, 0, 0}, a2[4] = {dv * q, du * q, 0, 0}; memcpy(stqf, a0, 16); memcpy(stqf + 4, a1, 16); memcpy(stqf + 8, a2, 16); }
            else { float a0[4] = {u0 * size, v0 * size, 1, 0}, a1[4] = {du * size, dv * size, 0, 0}, a2[4] = {dv * size, du * size, 0, 0}; memcpy(stqf, a0, 16); memcpy(stqf + 4, a1, 16); memcpy(stqf + 8, a2, 16); }
            memcpy(attrXB.ptr + 64ull * i, stqf, 48);
        }
        printf("   input: %u primitives (%u wide), %u textures (texture 7 disabled)\n", prims, wide, kTextures);

        // ---- descriptors
        VkDescriptorPoolSize ps2[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 2; dpci.poolSizeCount = 2; dpci.pPoolSizes = ps2;
        VkDescriptorPool pool;
        vkCreateDescriptorPool_(gDev, &dpci, nullptr, &pool);
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = pool; dsai.descriptorSetCount = 2; dsai.pSetLayouts = sl;
        VkDescriptorSet sets[2];
        vkAllocateDescriptorSets_(gDev, &dsai, sets);
        const Buf *bufs[6] = {&setupB, &attrXB, &attrB, &stateB, &texinfoB, &analysisB};
        VkDescriptorBufferInfo dbi[6];
        VkWriteDescriptorSet wr[6] = {};
        for (int i = 0; i < 6; ++i)
        {
            dbi[i] = {bufs[i]->buffer, 0, VK_WHOLE_SIZE};
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = i < 5 ? sets[0] : sets[1];
            wr[i].dstBinding = i < 5 ? b0[i] : 0;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = i < 5 ? t0[i] : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            wr[i].pBufferInfo = &dbi[i];
        }
        vkUpdateDescriptorSets_(gDev, 6, wr, 0, nullptr);

        // ---- dispatch
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = family;
        VkCommandPool cp;
        vkCreateCommandPool_(gDev, &cpi, nullptr, &cp);
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer cb;
        vkAllocateCommandBuffers_(gDev, &cbai, &cb);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer_(cb, &bi);
        vkCmdBindPipeline_(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets_(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, sets, 0, nullptr);
        int32_t pc[3] = {static_cast<int32_t>(prims), static_cast<int32_t>(kTextures), 1};
        vkCmdPushConstants_(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, pc);
        vkCmdDispatch_(cb, (prims + 255) / 256, 1, 1);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer_(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        printf("   vkQueueSubmit + wait ... "); fflush(stdout);
        vkQueueSubmit_(queue, 1, &si, nullptr);
        VkResult wr2 = vkQueueWaitIdle_(queue);
        printf("%s\n", wr2 == VK_SUCCESS ? "OK" : "FAILED (device lost?)");
        if (wr2 != VK_SUCCESS) { ++failures; continue; }

        // ---- canonical result
        uint64_t h = 1469598103934665603ull;
        uint64_t totalBlocks = 0, largest = 0;
        for (uint32_t t = 0; t < kTextures; ++t)
        {
            uint32_t d[3], valid;
            memcpy(d, disp[t].ptr, 12);
            memcpy(&valid, wg[t].ptr, 4);
            std::vector<uint64_t> idx;
            const uint32_t cnt = std::min<uint32_t>(d[0], kMaxWorkgroups);
            for (uint32_t i = 0; i < cnt; ++i) { uint64_t v; memcpy(&v, wg[t].ptr + 16 + 8ull * i, 8); idx.push_back(v); }
            std::sort(idx.begin(), idx.end());
            h = fnv(h, d, 12); h = fnv(h, &valid, 4); h = fnv(h, mask[t].ptr, 4ull * kBitmaskWords); h = fnv(h, idx.data(), idx.size() * 8);
            totalBlocks += d[0]; largest = std::max<uint64_t>(largest, d[0]);
            printf("   texture %u: dispatch {%u,%u,%u} valid %u\n", t, d[0], d[1], d[2], valid);
        }
        printf("   RESULT %s : total blocks %llu (largest texture %llu), digest %016llx\n", path, static_cast<unsigned long long>(totalBlocks), static_cast<unsigned long long>(largest),
               static_cast<unsigned long long>(h));
        if (totalBlocks == 0) { printf("   WARNING: the shader allocated nothing: the comparison is vacuous\n"); ++failures; }
        fflush(stdout);
    }
    return failures ? 1 : 0;
}
