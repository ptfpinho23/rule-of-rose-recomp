#include "gpuprobe.h"

#include <QByteArray>
#include <QList>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// The probe speaks to the Vulkan loader directly, with the few structures it
// needs written out here, so the launcher needs no Vulkan headers and has no
// link dependency on Vulkan. It applies the same rule as the game
// (recomp/rt/hw/gs_hw.c, hw_start): a device must offer Vulkan 1.3 with
// dualSrcBlend and dynamicRendering; the first such device is taken, and a
// discrete graphics card wins over it.

namespace {

#ifdef _WIN32
#define VKCALL __stdcall
#else
#define VKCALL
#endif

struct ApplicationInfo {            // VkApplicationInfo
    uint32_t sType; const void *pNext; const char *appName; uint32_t appVersion;
    const char *engineName; uint32_t engineVersion; uint32_t apiVersion;
};
struct InstanceCreateInfo {         // VkInstanceCreateInfo
    uint32_t sType; const void *pNext; uint32_t flags; const ApplicationInfo *appInfo;
    uint32_t layerCount; const char *const *layers; uint32_t extCount; const char *const *exts;
};
struct Features13 {                 // VkPhysicalDeviceVulkan13Features
    uint32_t sType; void *pNext; uint32_t f[15];      // dynamicRendering is f[12]
};
struct Features2 {                  // VkPhysicalDeviceFeatures2
    uint32_t sType; void *pNext; uint32_t f[55];      // dualSrcBlend is f[7]
};
// VkPhysicalDeviceProperties is 824 bytes; only its first fields are read:
// apiVersion at 0, deviceType at 16, deviceName (256 bytes) at 20.
struct Properties { alignas(8) unsigned char raw[2048]; };

typedef void *(VKCALL *GetInstanceProcAddr)(void *instance, const char *name);
typedef int32_t (VKCALL *CreateInstance)(const InstanceCreateInfo *, const void *, void **);
typedef void (VKCALL *DestroyInstance)(void *, const void *);
typedef int32_t (VKCALL *EnumeratePhysicalDevices)(void *, uint32_t *, void **);
typedef void (VKCALL *GetProperties)(void *, Properties *);
typedef void (VKCALL *GetFeatures2)(void *, Features2 *);

const uint32_t kApi13 = (1u << 22) | (3u << 12);

void *loaderSymbol()
{
#ifdef _WIN32
    HMODULE m = LoadLibraryA("vulkan-1.dll");
    return m ? reinterpret_cast<void *>(GetProcAddress(m, "vkGetInstanceProcAddr")) : nullptr;
#else
    void *m = dlopen("libvulkan.so.1", RTLD_NOW);
    return m ? dlsym(m, "vkGetInstanceProcAddr") : nullptr;
#endif
}

} // namespace

const char *GpuInfo::kindName(Kind k)
{
    switch (k) {
    case Cpu:        return "cpu";
    case Integrated: return "integrated";
    case Discrete:   return "discrete";
    default:         return "none";
    }
}

bool GpuInfo::parseKind(const QString &s, Kind *out)
{
    for (Kind k : {None, Cpu, Integrated, Discrete})
        if (s == QLatin1String(kindName(k))) {
            *out = k;
            return true;
        }
    return false;
}

int gpuProbeChildMain()
{
    GpuInfo::Kind kind = GpuInfo::None;
    char name[257] = "";
    do {
        auto gipa = reinterpret_cast<GetInstanceProcAddr>(loaderSymbol());
        if (!gipa)
            break;
        auto create = reinterpret_cast<CreateInstance>(gipa(nullptr, "vkCreateInstance"));
        if (!create)
            break;
        const ApplicationInfo ai = {0, nullptr, "launcher", 1, "probe", 1, kApi13};
        const InstanceCreateInfo ici = {1, nullptr, 0, &ai, 0, nullptr, 0, nullptr};
        void *inst = nullptr;
        if (create(&ici, nullptr, &inst) != 0 || !inst)
            break;
        auto enumerate = reinterpret_cast<EnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
        auto props = reinterpret_cast<GetProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
        auto feats = reinterpret_cast<GetFeatures2>(gipa(inst, "vkGetPhysicalDeviceFeatures2"));
        auto destroy = reinterpret_cast<DestroyInstance>(gipa(inst, "vkDestroyInstance"));
        void *dev[16] = {};
        uint32_t n = 16;
        if (enumerate && props && feats && enumerate(inst, &n, dev) >= 0) {
            bool have = false;
            for (uint32_t i = 0; i < n && i < 16; ++i) {
                static Properties p;
                memset(&p, 0, sizeof p);
                props(dev[i], &p);
                uint32_t api, type;
                memcpy(&api, p.raw, 4);
                memcpy(&type, p.raw + 16, 4);
                if (api < kApi13)
                    continue;
                Features13 f13 = {};
                f13.sType = 53;                      // ..._VULKAN_1_3_FEATURES
                Features2 f2 = {};
                f2.sType = 1000059000;               // ..._FEATURES_2
                f2.pNext = &f13;
                feats(dev[i], &f2);
                if (!f2.f[7] || !f13.f[12])
                    continue;
                if (have && type != 2)               // 2 = discrete: it wins, as in the game
                    continue;
                have = true;
                kind = type == 2 ? GpuInfo::Discrete : type == 4 ? GpuInfo::Cpu : GpuInfo::Integrated;
                memcpy(name, p.raw + 20, 256);
                name[256] = 0;
            }
        }
        if (destroy)
            destroy(inst, nullptr);
    } while (false);
    for (char *c = name; *c; ++c)
        if (*c == '\n' || *c == '\r')
            *c = ' ';
    printf("GPU %s %s\n", GpuInfo::kindName(kind), name);
    fflush(stdout);
    return 0;
}

GpuInfo parseGpuProbeOutput(const QByteArray &out)
{
    GpuInfo info;
    for (const QByteArray &raw : out.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (!line.startsWith("GPU "))
            continue;
        GpuInfo::Kind k;
        if (GpuInfo::parseKind(line.section(' ', 1, 1), &k)) {
            info.kind = k;
            info.name = k == GpuInfo::None ? QString() : line.section(' ', 2).trimmed();
        }
    }
    return info;
}
