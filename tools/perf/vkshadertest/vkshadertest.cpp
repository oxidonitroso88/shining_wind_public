// vkshadertest: standalone reproducer for "vkCreateShaderModule crashes inside the Vulkan driver" (no Granite, no parallel-gs, no game).
//
//   vkshadertest [--list] [--device N] [--bare-device] [--strip-decoration ID]... [--save-patched out.spv] file.spv [file.spv ...]
//
// Loads vulkan-1.dll, creates an instance + a device on the chosen physical device (enabling every Vulkan 1.1/1.2/1.3 core feature the GPU supports, like the
// game does) and calls ONLY vkCreateShaderModule for each file, reporting VK_SUCCESS or the access violation (module + offset of the faulting instruction,
// then the next file is tried: for a rigorous result run one file per process, as test_shaders.ps1 does).
// --strip-decoration ID removes every `OpDecorate <target> <ID>` before creating the module (ablation experiment; 5356 = AliasedPointer,
// 5355 = RestrictPointer). The input files are not modified.
#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static PFN_vkGetInstanceProcAddr gGetInstanceProcAddr;

static std::vector<uint32_t> readFile(const char *path)
{
    std::vector<uint32_t> out;
    FILE *f = fopen(path, "rb");
    if (!f)
        return out;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size + 3) / 4);
    if (fread(out.data(), 1, size, f) != static_cast<size_t>(size))
        out.clear();
    fclose(f);
    return out;
}

// Removes every `OpDecorate id <decoration> ...` instruction (the words are deleted, not turned into OpNop: OpNop outside a function body is invalid SPIR-V and
// would confound the experiment; SPIR-V has no absolute offsets, so deleting is safe). Returns how many were removed.
static int stripDecoration(std::vector<uint32_t> &code, uint32_t decoration)
{
    std::vector<uint32_t> out(code.begin(), code.begin() + 5);
    int count = 0;
    size_t i = 5;
    while (i < code.size())
    {
        const uint32_t op = code[i] & 0xFFFF, n = code[i] >> 16;
        if (n == 0 || i + n > code.size())
        {
            out.insert(out.end(), code.begin() + i, code.end()); // malformed tail: keep as is
            break;
        }
        if (op == 71 && n >= 3 && code[i + 2] == decoration) // OpDecorate
            ++count;
        else
            out.insert(out.end(), code.begin() + i, code.begin() + i + n);
        i += n;
    }
    code.swap(out);
    return count;
}

static const char *moduleOf(void *addr, char *buf, size_t size)
{
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(addr), &mod) && mod)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char *base = strrchr(path, '\\');
        snprintf(buf, size, "%s+0x%llx", base ? base + 1 : path, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
    }
    else
        snprintf(buf, size, "%p", addr);
    return buf;
}

static int gFaultCode = 0;
static void *gFaultAddress = nullptr;

static int onFault(EXCEPTION_POINTERS *info)
{
    gFaultCode = static_cast<int>(info->ExceptionRecord->ExceptionCode);
    gFaultAddress = info->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ objects with destructors here: __try/__except.
static bool createModule(PFN_vkCreateShaderModule fn, VkDevice device, const VkShaderModuleCreateInfo *info, VkResult *result, VkShaderModule *module)
{
    __try
    {
        *result = fn(device, info, nullptr, module);
        return true;
    }
    __except (onFault(GetExceptionInformation()))
    {
        return false;
    }
}

int main(int argc, char **argv)
{
    bool listOnly = false;
    uint32_t deviceIndex = 0;
    std::vector<uint32_t> strip;
    const char *savePatched = nullptr;
    bool bareDevice = false;
    std::vector<const char *> files;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--list"))
            listOnly = true;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc)
            deviceIndex = static_cast<uint32_t>(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--bare-device"))
            bareDevice = true;
        else if (!strcmp(argv[i], "--save-patched") && i + 1 < argc)
            savePatched = argv[++i];
        else if (!strcmp(argv[i], "--strip-decoration") && i + 1 < argc)
            strip.push_back(static_cast<uint32_t>(atoi(argv[++i])));
        else
            files.push_back(argv[i]);
    }

    HMODULE loader = LoadLibraryA("vulkan-1.dll");
    if (!loader)
    {
        printf("vulkan-1.dll not found\n");
        return 2;
    }
    gGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gGetInstanceProcAddr(nullptr, "vkCreateInstance"));

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = nullptr;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
    {
        app.apiVersion = VK_API_VERSION_1_1;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
        {
            printf("vkCreateInstance failed\n");
            return 2;
        }
    }
#define INST(name) auto name = reinterpret_cast<PFN_##name>(gGetInstanceProcAddr(instance, #name))
    INST(vkEnumeratePhysicalDevices);
    INST(vkGetPhysicalDeviceProperties);
    INST(vkGetPhysicalDeviceFeatures2);
    INST(vkGetPhysicalDeviceQueueFamilyProperties);
    INST(vkCreateDevice);
    INST(vkGetDeviceProcAddr);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    for (uint32_t i = 0; i < count; ++i)
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devices[i], &p);
        printf("device %u: %s (vendor 0x%04x, driver 0x%08x, api %u.%u.%u)%s\n", i, p.deviceName, p.vendorID, p.driverVersion, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), i == deviceIndex ? "  <- selected" : "");
    }
    if (listOnly || files.empty())
        return 0;
    if (deviceIndex >= count)
    {
        printf("no such device\n");
        return 2;
    }

    VkPhysicalDevice phys = devices[deviceIndex];
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feats.pNext = &f11;
    f11.pNext = &f12;
    if (props.apiVersion >= VK_API_VERSION_1_3)
        f12.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(phys, &feats); // what the GPU supports: enable all of it
    printf("features: shaderInt16=%d storageBuffer16=%d uniformAndStorageBuffer16=%d storageBuffer8=%d bufferDeviceAddress=%d descriptorIndexing=%d\n", feats.features.shaderInt16,
           f11.storageBuffer16BitAccess, f11.uniformAndStorageBuffer16BitAccess, f12.storageBuffer8BitAccess, f12.bufferDeviceAddress, f12.descriptorIndexing);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
    uint32_t family = 0;
    for (uint32_t i = 0; i < nq; ++i)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
        {
            family = i;
            break;
        }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = bareDevice ? nullptr : &feats; // --bare-device: no feature enabled at all (to see whether the enabled features matter)
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device = nullptr;
    if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS)
    {
        printf("vkCreateDevice failed\n");
        return 2;
    }
    auto vkCreateShaderModuleFn = reinterpret_cast<PFN_vkCreateShaderModule>(vkGetDeviceProcAddr(device, "vkCreateShaderModule"));

    int failures = 0;
    for (const char *path : files)
    {
        std::vector<uint32_t> code = readFile(path);
        if (code.size() < 5 || code[0] != 0x07230203u)
        {
            printf("%s: not SPIR-V\n", path);
            ++failures;
            continue;
        }
        for (uint32_t d : strip)
            printf("%s: removed %d OpDecorate(%u)\n", path, stripDecoration(code, d), d);
        if (savePatched) // the exact words handed to the driver, for spirv-val / spirv-dis
            if (FILE *out = fopen(savePatched, "wb"))
            {
                fwrite(code.data(), 4, code.size(), out);
                fclose(out);
            }
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * 4;
        info.pCode = code.data();
        printf("%s: vkCreateShaderModule (%zu bytes) ... ", path, info.codeSize);
        fflush(stdout);
        VkResult result = VK_SUCCESS;
        VkShaderModule module = nullptr;
        if (createModule(vkCreateShaderModuleFn, device, &info, &result, &module))
        {
            printf("%s (VkResult %d)\n", result == VK_SUCCESS ? "OK" : "FAILED", static_cast<int>(result));
            failures += result != VK_SUCCESS;
        }
        else
        {
            char where[256];
            printf("CRASH 0x%08X at %s\n", gFaultCode, moduleOf(gFaultAddress, where, sizeof(where)));
            ++failures;
        }
        fflush(stdout);
    }
    return failures ? 1 : 0;
}
