/* vkprobe.c — loads Vulkan dynamically (no link dependency) and lists the
 * devices with the features the planned GS hardware renderer needs.
 * Linux:   clang -Itools/vulkan/Vulkan-Headers-1.3.290/include recomp/tools/vkprobe.c -o vkprobe
 * Windows: tools/llvm-mingw/bin/x86_64-w64-mingw32-clang (same arguments) -o vkprobe.exe */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void *lib_open(void) { return LoadLibraryA("vulkan-1.dll"); }
static void *lib_sym(void *l, const char *n) { return (void *)GetProcAddress((HMODULE)l, n); }
#else
#include <dlfcn.h>
static void *lib_open(void) { return dlopen("libvulkan.so.1", RTLD_NOW); }
static void *lib_sym(void *l, const char *n) { return dlsym(l, n); }
#endif
int main(void) {
    void *l = lib_open();
    if (!l) { printf("no Vulkan loader\n"); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)lib_sym(l, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkprobe", 1, "none", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai };
    VkInstance inst;
    if (ci(&ici, 0, &inst) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return 1; }
    PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures2 gf = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
    VkPhysicalDevice pd[8]; uint32_t n = 8;
    epd(inst, &n, pd);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; gp(pd[i], &p);
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
        if (p.apiVersion >= VK_API_VERSION_1_1) gf(pd[i], &f2);
        printf("device %u: %s, type %d, Vulkan %u.%u.%u, dualSrcBlend %u, dynamicRendering %u, maxImageDimension2D %u, maxPushConstantsSize %u\n",
               i, p.deviceName, p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               f2.features.dualSrcBlend, f13.dynamicRendering, p.limits.maxImageDimension2D, p.limits.maxPushConstantsSize);
    }
    return 0;
}
