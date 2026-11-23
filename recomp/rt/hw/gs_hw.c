/* gs_hw.c — Vulkan layer of the GS hardware renderer.
 *
 * Vulkan 1.3 (dynamic rendering, dual-source blending), loaded at run time
 * through a function table: no link dependency, hw_start() simply fails on a
 * machine without it. Everything is recorded into one command buffer that is
 * submitted and waited for at hw_flush() (once per displayed frame, or when
 * the caller needs pixels back).
 *
 * Shaders: gs.vert / gs.frag / blit.vert / blit.frag in this directory,
 * compiled into gs_hw_spv.h by recomp/tools/gshw-shaders.sh. */
#define VK_NO_PROTOTYPES
#include "../../../tools/vulkan/Vulkan-Headers-1.3.290/include/vulkan/vulkan_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include "gs_hw.h"
#include "gs_hw_spv.h"
#undef hw_flush
#define hw_flush() hw_flush_at(10000 + __LINE__)       /* sites in this file: 10000 + line */
/* Threads: the game thread draws; with frame interpolation a second thread
 * (the presenter) replays draw lists into its own targets and owns the
 * swapchain. Everything a thread records with is thread-local (command
 * buffer, vertex/staging/read-back buffers, batch and render-pass state).
 * Shared: the queue (qmx around submit/present/wait-idle), the pipeline cache
 * (pmx), and images that are only read once handed over (textures, saved
 * target copies). Images and descriptor sets are created and destroyed by the
 * game thread only. */
#define TL __thread
static pthread_mutex_t qm[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER }, pmx = PTHREAD_MUTEX_INITIALIZER, dmx = PTHREAD_MUTEX_INITIALIZER;     /* dmx: descriptor pool */
/* Each thread submits to its own queue when the device has two (so a present
 * that waits for the display never holds up the game thread's submits); with
 * one queue both use queue 0 under qm[0]. */
static VkQueue queues[2]; static int nqueues; static TL int qi;
#ifdef _WIN32
#include <windows.h>
#define PLATFORM_SURFACE_EXT "VK_KHR_win32_surface"
#define PLATFORM_SURFACE_FN "vkCreateWin32SurfaceKHR"
#define PLATFORM_SURFACE_STYPE 1000009000
static void *lib_open(void) { return LoadLibraryA("vulkan-1.dll"); }
static void *lib_sym(void *l, const char *n) { return (void *)GetProcAddress((HMODULE)l, n); }
#else
#include <dlfcn.h>
#define PLATFORM_SURFACE_EXT "VK_KHR_xlib_surface"
#define PLATFORM_SURFACE_FN "vkCreateXlibSurfaceKHR"
#define PLATFORM_SURFACE_STYPE 1000004000
static void *lib_open(void) { return dlopen("libvulkan.so.1", RTLD_NOW); }
static void *lib_sym(void *l, const char *n) { return dlsym(l, n); }
#endif

#define VKFN(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFeatures2) X(CreateDevice) X(GetDeviceProcAddr) \
    X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceSurfacePresentModesKHR)
#define VKDFN(X) \
    X(GetDeviceQueue) X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(QueueSubmit) \
    X(CreateFence) X(WaitForFences) X(ResetFences) X(ResetCommandBuffer) X(CreateImage) X(GetImageMemoryRequirements) \
    X(AllocateMemory) X(BindImageMemory) X(CreateImageView) X(DestroyImage) X(DestroyImageView) X(FreeMemory) X(CreateBuffer) \
    X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(CreateSampler) X(CreateDescriptorSetLayout) \
    X(CreateDescriptorPool) X(AllocateDescriptorSets) X(FreeDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) \
    X(CreateShaderModule) X(CreateGraphicsPipelines) X(CmdBeginRendering) X(CmdEndRendering) X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw) \
    X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdBlitImage) \
    X(CmdClearDepthStencilImage) X(CmdSetBlendConstants) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
    X(AcquireNextImageKHR) X(QueuePresentKHR) X(CreateSemaphore) X(DeviceWaitIdle) X(CmdClearColorImage)
#define X(n) static PFN_vk##n vk##n;
VKFN(X) VKDFN(X)
#undef X

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkImageLayout layout; VkDescriptorSet set, set_lin; int w, h, depth; } Img;
#define HW_SLOTS 4
typedef struct { Img col, copy, up, sv[HW_SLOTS], fin[HW_SLOTS]; int w, h; uint64_t ver, copy_ver; int n, mv; } Rt;        /* w, h native; col/copy at device size (n = its scale: N except the FSR 3.1 2D layer), up native; mv: motion-vector target */
typedef struct { float rt[4]; uint32_t flags; int32_t aref; uint32_t fogcol, blend; int32_t region[4]; float tex[4], zp[4]; uint32_t fbmsk; } PushC;

#define MAX_RT 32
#define MAX_TEX 8192
#define VB_BYTES (48u << 20)
#define ST_BYTES (96u << 20)
static VkInstance inst; static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue; static uint32_t qfam;
static TL VkCommandPool pool; static TL VkCommandBuffer cb; static TL VkFence fence; static TL int is_game;
static VkPhysicalDeviceMemoryProperties memprops; static char devname[256];
static PFN_vkGetInstanceProcAddr gipa; static int have_surface_ext, have_swapchain_ext;
static VkSurfaceKHR surf; static VkSwapchainKHR swp; static VkImage swimg[8]; static VkImageView swview[8]; static uint32_t nsw;
static VkFormat swfmt; static VkExtent2D swext; static VkSemaphore sem_acq, sem_done; static int sw_vsync;
static VkSampler sampler_lin; static Img lut_img; static int lut_on;
static float pr_rcas; static int pr_bloom, sw_can_grab;        /* presentation options (hw_present_opts) */
static TL Img st_a, st_b, gl_a, gl_b;                          /* presentation stages of the presenting thread */
static TL int grab_want, grab_w, grab_h; static TL size_t grab_at;
static VkSampler sampler; static VkDescriptorSetLayout dsl; static VkDescriptorPool dpool; static VkPipelineLayout playout;
static VkShaderModule sm_vert, sm_frag, sm_bvert, sm_bfrag, sm_pfrag, sm_mvvert, sm_mvfrag, sm_ffrag; static VkPipeline blit_pipe;
static int fsr_want; static float mv_zfar, mv_zinv;      /* FSR 3.1 asked for (hw_fsr3_want); depth mapping of the motion-vector pass */
static int N = 1;
static float uv_shift_k(void) { static float k = -1.0f; if (k < 0.0f) k = getenv("ROR_HW_UVSHIFT") ? (float)atof(getenv("ROR_HW_UVSHIFT")) : 0.0f; return k; }
/* Texture coordinates of triangles are taken at the fragment itself (native position g / N), where the
 * rasterizer interpolates inside the triangle. Until 2026-11-23 they were shifted to (g + 0.5) / N - 0.5
 * (ROR_HW_UVSHIFT=1 brings that back): up to 0.375 native pixels outside the top/left edges at 4x, which
 * wrapped into the far side of repeating textures and drew hairlines along the edges of alpha-tested
 * grass and foliage polygons. No effect at --internal 1. */
static Rt rts[MAX_RT]; static int nrt;
static Img dss[MAX_RT]; static int nds;
static Img dsv[MAX_RT][HW_SLOTS];                 /* depth as a frame found it (frame interpolation) */
static Img texs[MAX_TEX]; static uint8_t tex_used[MAX_TEX]; static int tex_dead[MAX_TEX], ntex_dead; static uint64_t tex_dead_gen[MAX_TEX], keep_gen;
static Img dummy;
static TL VkBuffer vbuf, sbuf, rbuf; static TL uint8_t *vmap, *smap, *rmap; static TL VkDeviceMemory rmem; static TL size_t voff, soff, rcap, roff, st_cap;
static TL int rendering, cur_rt = -1, cur_ds = -1, recording;
static struct { uint32_t key; VkPipeline p; } pipes[256]; static int npipes;
static struct { size_t off; int w, h; } tickets[256]; static int ntickets;
uint64_t hw_stat_draws, hw_stat_copies, hw_stat_flushes, hw_stat_wait_ns;
static int exact_afail;
/* pending batch */
static TL HwState bstate; static TL size_t bfirst; static TL int bcount;

static void die(const char *what, int r) { fprintf(stderr, "[gs_hw] %s failed (%d)\n", what, r); exit(1); }
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) die(#call, (int)r_); } while (0)

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    die("memory type", 0); return 0;
}
static void begin_cb(void) {
    if (recording) return;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0 };
    CHECK(vkBeginCommandBuffer(cb, &bi));
    recording = 1;
}
static void end_rendering(void) { if (rendering) { vkCmdEndRendering(cb); rendering = 0; } }
static void to_layout(Img *im, VkImageLayout nl) {       /* outside rendering only */
    if (im->layout == nl) return;
    begin_cb();
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im->layout; b.newLayout = nl;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->img;
    b.subresourceRange = (VkImageSubresourceRange){ im->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
    im->layout = nl;
}
static void img_create(Img *im, int w, int h, int depth, int sampled) {
    memset(im, 0, sizeof *im);
    im->w = w; im->h = h; im->depth = depth;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    int f16 = depth == 2;                         /* 2: half-float colour (presentation stage) */
    if (f16) { depth = 0; im->depth = 0; }
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = depth ? VK_FORMAT_D32_SFLOAT : f16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = (VkExtent3D){ (uint32_t)w, (uint32_t)h, 1 }; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = depth ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
                     : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(dev, &ci, 0, &im->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECK(vkAllocateMemory(dev, &ai, 0, &im->mem));
    CHECK(vkBindImageMemory(dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
    vi.subresourceRange = (VkImageSubresourceRange){ depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    CHECK(vkCreateImageView(dev, &vi, 0, &im->view));
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (sampled) {
        VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dpool, 1, &dsl };
        pthread_mutex_lock(&dmx);
        VkResult ar_ = vkAllocateDescriptorSets(dev, &da, &im->set);
        pthread_mutex_unlock(&dmx);
        CHECK(ar_);
        VkDescriptorImageInfo ii = { sampler, im->view, depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, im->set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, 0, 0 };
        vkUpdateDescriptorSets(dev, 1, &wd, 0, 0);
    }
}
static void img_add_lin(Img *im) {               /* a second descriptor set with the bilinear sampler */
    VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dpool, 1, &dsl };
    pthread_mutex_lock(&dmx);
    VkResult ar_ = vkAllocateDescriptorSets(dev, &da, &im->set_lin);
    pthread_mutex_unlock(&dmx);
    CHECK(ar_);
    VkDescriptorImageInfo ii = { sampler_lin, im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, im->set_lin, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, 0, 0 };
    vkUpdateDescriptorSets(dev, 1, &wd, 0, 0);
}
static void img_destroy(Img *im) {
    pthread_mutex_lock(&dmx);
    if (im->set) vkFreeDescriptorSets(dev, dpool, 1, &im->set);
    if (im->set_lin) vkFreeDescriptorSets(dev, dpool, 1, &im->set_lin);
    pthread_mutex_unlock(&dmx);
    vkDestroyImageView(dev, im->view, 0); vkDestroyImage(dev, im->img, 0); vkFreeMemory(dev, im->mem, 0);
    memset(im, 0, sizeof *im);
}
static void buf_create(VkBuffer *b, VkDeviceMemory *m, uint8_t **map, size_t size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, 0 };
    CHECK(vkCreateBuffer(dev, &ci, 0, b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, *b, &mr);
    /* read-back buffers want cached memory (reading write-combined memory is very slow) */
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
        for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
            if ((mr.memoryTypeBits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & (want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) == (want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) { want |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT; break; }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(mr.memoryTypeBits, want) };
    CHECK(vkAllocateMemory(dev, &ai, 0, m));
    CHECK(vkBindBufferMemory(dev, *b, *m, 0));
    void *p; CHECK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, &p)); *map = p;
}
static VkShaderModule shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, bytes, code };
    VkShaderModule m; CHECK(vkCreateShaderModule(dev, &ci, 0, &m)); return m;
}

/* key: blend_mode(2) blend_const(1) cmask(4) ztest(2) zwrite(1) blit(1) present(1) format(3, presentation passes)
 * mv(1: motion-vector variant of the GS shaders, RGBA32F target) fsr3(1: fsr3.frag instead of present.frag) */
static VkPipeline pipeline_new(uint32_t key);
static VkPipeline pipeline(uint32_t key) {
    static TL struct { uint32_t key; VkPipeline p; } last[8]; static TL int ln;         /* per-thread shortcut past the lock */
    for (int i = 0; i < 8; i++) if (last[i].p && last[i].key == key) return last[i].p;
    pthread_mutex_lock(&pmx);
    VkPipeline p = pipeline_new(key);
    pthread_mutex_unlock(&pmx);
    last[ln & 7].key = key; last[ln++ & 7].p = p;
    return p;
}
static VkPipeline pipeline_new(uint32_t key) {
    for (int i = 0; i < npipes; i++) if (pipes[i].key == key) return pipes[i].p;
    int bm = key & 3, bconst = (key >> 2) & 1, cmask = (key >> 3) & 15, ztest = (key >> 7) & 3, zwrite = (key >> 9) & 1, pres = (key >> 11) & 1, blit = ((key >> 10) & 1) | pres;
    int mv = (key >> 15) & 1, f3 = (key >> 16) & 1;
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, blit ? sm_bvert : mv ? sm_mvvert : sm_vert, "main", 0 },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, f3 ? sm_ffrag : pres ? sm_pfrag : blit ? sm_bfrag : mv ? sm_mvfrag : sm_frag, "main", 0 } };
    VkVertexInputBindingDescription vb = { 0, sizeof(HwVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[4] = { { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 16 },
                                                { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 28 }, { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 44 } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, 0, 0, blit ? 0u : 1u, &vb, blit ? 0u : 4u, va };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0 };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0 };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dz = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dz.depthTestEnable = ztest != 0; dz.depthWriteEnable = zwrite;
    dz.depthCompareOp = ztest == 2 ? VK_COMPARE_OP_GREATER_OR_EQUAL : ztest == 3 ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = (VkColorComponentFlags)cmask;
    if (bm != HWB_OFF) {
        VkBlendFactor f = bconst ? VK_BLEND_FACTOR_CONSTANT_COLOR : VK_BLEND_FACTOR_SRC1_COLOR;      /* (fsr3 passes: additive with the constant at 1) */
        ba.blendEnable = 1;
        ba.srcColorBlendFactor = f;
        ba.dstColorBlendFactor = bm == HWB_MIX ? (bconst ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR) : VK_BLEND_FACTOR_ONE;
        ba.colorBlendOp = bm == HWB_REVSUB ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1; cbs.pAttachments = &ba;
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 3, dyn };
    /* presentation passes: bits 12-14 = target (0 swapchain, 1 RGBA8 image, 2 half-float image; FSR 3.1 inputs: 4 R32F, 5 R8, 6 RG16F) */
    static const VkFormat pf[8] = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM };
    VkFormat cf = !pres ? (mv ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM) : ((key >> 12) & 7) == 0 ? swfmt : pf[(key >> 12) & 7];
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &cf, blit ? VK_FORMAT_UNDEFINED : VK_FORMAT_D32_SFLOAT, VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri };
    pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
    pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &dz; pi.pColorBlendState = &cbs;
    pi.pDynamicState = &ds; pi.layout = playout;
    VkPipeline p; CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, 0, &p));
    if (npipes < 256) { pipes[npipes].key = key; pipes[npipes++].p = p; }
    return p;
}

const char *hw_device_name(void) { return devname; }
/* Per-thread recording state: called by hw_start for the game thread and by
 * any other thread before it uses the layer. */
void hw_thread_init(void) {
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam };
    CHECK(vkCreateCommandPool(dev, &pci, 0, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(dev, &fi, 0, &fence));
    VkDeviceMemory m;
    buf_create(&vbuf, &m, &vmap, VB_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    st_cap = is_game ? ST_BYTES : (1u << 20);
    buf_create(&sbuf, &m, &smap, st_cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    rcap = 80u << 20;
    buf_create(&rbuf, &rmem, &rmap, rcap, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    cur_rt = cur_ds = -1;
    qi = (!is_game && nqueues > 1) ? 1 : 0;
}
int hw_start(int scale) {
    void *l = lib_open();
    if (!l) return 0;
    gipa = (PFN_vkGetInstanceProcAddr)lib_sym(l, "vkGetInstanceProcAddr");
    if (!gipa) return 0;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enumext = (PFN_vkEnumerateInstanceExtensionProperties)gipa(0, "vkEnumerateInstanceExtensionProperties");
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "gs", 1, "gs_hw", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &ai };
    static const char *const want_ext[2] = { "VK_KHR_surface", PLATFORM_SURFACE_EXT };
    {   static VkExtensionProperties ep[256]; uint32_t ne = 256; int got = 0;
        if (enumext && enumext(0, &ne, ep) >= 0) for (uint32_t i = 0; i < ne; i++) for (int k = 0; k < 2; k++) if (!strcmp(ep[i].extensionName, want_ext[k])) got |= 1 << k;
        if (got == 3) { ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = want_ext; have_surface_ext = 1; }
    }
    if (!create || create(&ici, 0, &inst) != VK_SUCCESS) return 0;
#define X(n) vk##n = (PFN_vk##n)gipa(inst, "vk" #n);
    VKFN(X)
#undef X
    VkPhysicalDevice pd[16]; uint32_t n = 16;
    vkEnumeratePhysicalDevices(inst, &n, pd);
    int best = -1, want = getenv("ROR_VK_DEVICE") ? atoi(getenv("ROR_VK_DEVICE")) : -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
        vkGetPhysicalDeviceFeatures2(pd[i], &f2);
        if (!f2.features.dualSrcBlend || !f13.dynamicRendering) continue;
        if (want >= 0 ? (int)i == want : (best < 0 || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) { best = (int)i; snprintf(devname, sizeof devname, "%s", p.deviceName); }
    }
    if (best < 0) return 0;
    phys = pd[best];
    VkQueueFamilyProperties qf[16]; uint32_t nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    qfam = 0;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    float prio[2] = { 1.0f, 1.0f };
    nqueues = qf[qfam].queueCount >= 2 ? 2 : 1;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, qfam, (uint32_t)nqueues, prio };
    VkPhysicalDeviceVulkan13Features e13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    e13.dynamicRendering = 1;
    VkPhysicalDeviceFeatures2 ef = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e13 };
    ef.features.dualSrcBlend = 1;
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef };
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    static const char *dev_ext[4] = { "VK_KHR_swapchain" };
    /* FSR 3.1 (only when asked for; the device is created exactly as before otherwise): the
     * FidelityFX Vulkan backend picks its shader variants from what the physical device
     * supports (half floats, 16-bit integers, wave size control), so what is supported of
     * that list is enabled here. The cooperative-matrix and 8-bit-float extensions are what
     * AMD's sample framework additionally enables for the ML upscaler (FSR 4); they are
     * enabled when present and are not used by anything else. */
    VkPhysicalDeviceVulkan11Features e11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features e12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR ecm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    struct { VkStructureType sType; void *pNext; VkBool32 shaderFloat8; } ef8 = { (VkStructureType)1000567000 /* PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT */, 0, 0 };
    {   static VkExtensionProperties ep[512]; uint32_t ne = 512; int have_cm = 0, have_f8 = 0, nx = 0;
        if (vkEnumerateDeviceExtensionProperties(phys, 0, &ne, ep) >= 0)
            for (uint32_t i = 0; i < ne; i++) {
                if (have_surface_ext && !strcmp(ep[i].extensionName, dev_ext[0])) have_swapchain_ext = 1;
                if (!strcmp(ep[i].extensionName, "VK_KHR_cooperative_matrix")) have_cm = 1;
                if (!strcmp(ep[i].extensionName, "VK_EXT_shader_float8")) have_f8 = 1;
            }
        if (have_swapchain_ext) nx = 1;
        if (fsr_want) {
            VkPhysicalDeviceVulkan13Features s13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            VkPhysicalDeviceVulkan12Features s12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &s13 };
            VkPhysicalDeviceVulkan11Features s11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &s12 };
            VkPhysicalDeviceFeatures2 sf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s11 };
            if (have_cm) { ecm.pNext = s13.pNext; s13.pNext = &ecm; }
            if (have_f8) { ef8.pNext = s13.pNext; s13.pNext = &ef8; }
            vkGetPhysicalDeviceFeatures2(phys, &sf);
            ef.features.shaderImageGatherExtended = sf.features.shaderImageGatherExtended;
            ef.features.shaderStorageImageWriteWithoutFormat = sf.features.shaderStorageImageWriteWithoutFormat;
            ef.features.shaderStorageImageExtendedFormats = sf.features.shaderStorageImageExtendedFormats;
            ef.features.shaderInt16 = sf.features.shaderInt16;
            e11.storageBuffer16BitAccess = s11.storageBuffer16BitAccess;
            e12.shaderFloat16 = s12.shaderFloat16; e12.shaderInt8 = s12.shaderInt8;
            e13.synchronization2 = s13.synchronization2; e13.subgroupSizeControl = s13.subgroupSizeControl; e13.computeFullSubgroups = s13.computeFullSubgroups;
            e13.shaderDemoteToHelperInvocation = s13.shaderDemoteToHelperInvocation; e13.maintenance4 = s13.maintenance4;
            e13.pNext = &e11; e11.pNext = &e12;
            if (have_cm && ecm.cooperativeMatrix) { ecm.cooperativeMatrixRobustBufferAccess = 0; ecm.pNext = e12.pNext; e12.pNext = &ecm; dev_ext[nx++] = "VK_KHR_cooperative_matrix"; }
            if (have_f8 && ef8.shaderFloat8) { ef8.pNext = e12.pNext; e12.pNext = &ef8; dev_ext[nx++] = "VK_EXT_shader_float8"; }
        }
        if (nx) { di.enabledExtensionCount = (uint32_t)nx; di.ppEnabledExtensionNames = dev_ext; }
    }
    if (vkCreateDevice(phys, &di, 0, &dev) != VK_SUCCESS) return 0;
#define X(n) vk##n = (PFN_vk##n)vkGetDeviceProcAddr(dev, "vk" #n);
    VKDFN(X)
#undef X
    N = scale < 1 ? 1 : scale > 8 ? 8 : scale;
    exact_afail = getenv("ROR_HW_EXACT_AFAIL") != 0;
    vkGetDeviceQueue(dev, qfam, 0, &queues[0]); queues[1] = queues[0];
    if (nqueues > 1) vkGetDeviceQueue(dev, qfam, 1, &queues[1]);
    queue = queues[0];
    is_game = 1;
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(vkCreateSampler(dev, &si, 0, &sampler));
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    CHECK(vkCreateSampler(dev, &si, 0, &sampler_lin));
    VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &lb };
    CHECK(vkCreateDescriptorSetLayout(dev, &li, 0, &dsl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEX + 8 * MAX_RT + 8 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, MAX_TEX + 8 * MAX_RT + 8, 1, &ps };
    CHECK(vkCreateDescriptorPool(dev, &dpi, 0, &dpool));
    VkDescriptorSetLayout two[2] = { dsl, dsl };
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushC) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 2, two, 1, &pr };
    CHECK(vkCreatePipelineLayout(dev, &pli, 0, &playout));
    sm_vert = shader(spv_gs_vert, sizeof spv_gs_vert); sm_frag = shader(spv_gs_frag, sizeof spv_gs_frag);
    sm_bvert = shader(spv_blit_vert, sizeof spv_blit_vert); sm_bfrag = shader(spv_blit_frag, sizeof spv_blit_frag);
    sm_pfrag = shader(spv_present_frag, sizeof spv_present_frag);
    if (fsr_want) { sm_mvvert = shader(spv_mv_vert, sizeof spv_mv_vert); sm_mvfrag = shader(spv_mv_frag, sizeof spv_mv_frag); sm_ffrag = shader(spv_fsr3_frag, sizeof spv_fsr3_frag); }
    hw_thread_init();
    blit_pipe = pipeline((15u << 3) | (1u << 10));
    img_create(&dummy, 1, 1, 0, 1);
    to_layout(&dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

static void emit_batch(void);
static void submit_cb(VkSemaphore wait, VkSemaphore signal) {
    emit_batch();
    if (!recording) return;
    end_rendering();
    CHECK(vkEndCommandBuffer(cb));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, wait ? 1u : 0u, &wait, &ws, 1, &cb, signal ? 1u : 0u, &signal };
    pthread_mutex_lock(&qm[qi]);
    VkResult sr = vkQueueSubmit(queues[qi], 1, &si, fence);
    pthread_mutex_unlock(&qm[qi]);
    CHECK(sr);
    {   struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        CHECK(vkWaitForFences(dev, 1, &fence, 1, ~0ull));
        clock_gettime(CLOCK_MONOTONIC, &b);
        if (is_game) hw_stat_wait_ns += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
    }
    CHECK(vkResetFences(dev, 1, &fence));
    CHECK(vkResetCommandBuffer(cb, 0));
    recording = 0; voff = soff = 0; cur_rt = cur_ds = -1;
    if (is_game) {           /* textures given up: destroy once no retained draw list can still use them */
        int n = 0;
        for (int i = 0; i < ntex_dead; i++) {
            if (keep_gen && keep_gen - tex_dead_gen[i] < 5) { tex_dead[n] = tex_dead[i]; tex_dead_gen[n++] = tex_dead_gen[i]; continue; }
            img_destroy(&texs[tex_dead[i]]); tex_used[tex_dead[i]] = 0;
        }
        ntex_dead = n;
    }
    hw_stat_flushes++;
}
int hw_flush_sites[16], hw_flush_counts[16];
void hw_flush_at(int site) {
    if (is_game && recording) for (int i = 0; i < 16; i++) if (hw_flush_sites[i] == site || !hw_flush_sites[i]) { hw_flush_sites[i] = site; hw_flush_counts[i]++; break; }
    submit_cb(VK_NULL_HANDLE, VK_NULL_HANDLE);
}

/* ---- presentation ------------------------------------------------------------- */
static void upload_img(Img *im, int x, int y, int w, int h, const uint32_t *rgba);
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *a; void *b; } SurfaceInfo;   /* Xlib: dpy, window; Win32: hinstance, hwnd */
static int swap_create(int w, int h) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps) != VK_SUCCESS) return 0;
    VkExtent2D e = caps.currentExtent;
    if (e.width == 0xFFFFFFFFu) { e.width = (uint32_t)w; e.height = (uint32_t)h; }
    if (e.width == 0 || e.height == 0) return 0;                 /* minimised */
    static VkSurfaceFormatKHR fm[64]; uint32_t nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, fm);
    VkSurfaceFormatKHR f = fm[0];
    for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { f = fm[i]; break; }
    VkPresentModeKHR pm[16]; uint32_t np = 16, have = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &np, pm);
    for (uint32_t i = 0; i < np; i++) have |= 1u << pm[i];
    /* vsync: MAILBOX (tear-free, never blocks the caller) if there is one, else FIFO; no vsync: IMMEDIATE */
    /* sw_vsync: 1 = tear-free without blocking the caller (MAILBOX, else FIFO); 2 = FIFO, the caller is paced by the
     * display (presenter thread); 0 = IMMEDIATE */
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (sw_vsync == 1) { if (have & (1u << VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR; }
    else if (sw_vsync == 0) {
        if (have & (1u << VK_PRESENT_MODE_IMMEDIATE_KHR)) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (have & (1u << VK_PRESENT_MODE_MAILBOX_KHR)) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = surf; ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = f.format; ci.imageColorSpace = f.colorSpace; ci.imageExtent = e; ci.imageArrayLayers = 1;
    sw_can_grab = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (sw_can_grab ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0); ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode; ci.clipped = 1; ci.oldSwapchain = swp;
    VkSwapchainKHR ns;
    if (vkCreateSwapchainKHR(dev, &ci, 0, &ns) != VK_SUCCESS) return 0;
    if (swp) { for (uint32_t i = 0; i < nsw; i++) vkDestroyImageView(dev, swview[i], 0); vkDestroySwapchainKHR(dev, swp, 0); }
    swp = ns; nsw = 8;
    vkGetSwapchainImagesKHR(dev, swp, &nsw, swimg);
    if (swfmt != f.format) { swfmt = f.format; for (int i = 0; i < npipes; i++) if (pipes[i].key & (1u << 11)) pipes[i].key = ~0u; }
    swext = e;
    for (uint32_t i = 0; i < nsw; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = swimg[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = swfmt;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        CHECK(vkCreateImageView(dev, &vi, 0, &swview[i]));
    }
    fprintf(stderr, "[gs_hw] swapchain %ux%u, %u images, present mode %s\n", e.width, e.height, nsw,
            mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
    return 1;
}
int hw_swap_attach(void *a, void *b, int vsync) {
    if (!have_swapchain_ext) return 0;
    typedef VkResult (VKAPI_PTR *CreateSurf)(VkInstance, const SurfaceInfo *, const VkAllocationCallbacks *, VkSurfaceKHR *);
    CreateSurf cs = (CreateSurf)gipa(inst, PLATFORM_SURFACE_FN);
    SurfaceInfo si = { (VkStructureType)PLATFORM_SURFACE_STYPE, 0, 0, a, b };
    if (!cs || cs(inst, &si, 0, &surf) != VK_SUCCESS) return 0;
    VkBool32 ok = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, surf, &ok);
    if (!ok) return 0;
    sw_vsync = vsync;
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK(vkCreateSemaphore(dev, &sci, 0, &sem_acq)); CHECK(vkCreateSemaphore(dev, &sci, 0, &sem_done));
    return swap_create(640, 480);
}
void hw_set_lut(const uint8_t *lut) {
    lut_on = lut != 0;
    if (!lut) return;
    uint32_t px[256];
    for (int i = 0; i < 256; i++) px[i] = lut[i] * 0x010101u | 0xFF000000u;
    emit_batch();
    if (!lut_img.img) img_create(&lut_img, 256, 1, 0, 1);
    upload_img(&lut_img, 0, 0, 256, 1, px);
    to_layout(&lut_img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
static void sw_barrier(VkImage im, VkImageLayout a, VkImageLayout b) {
    VkImageMemoryBarrier br = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    br.srcAccessMask = br.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    br.oldLayout = a; br.newLayout = b; br.srcQueueFamilyIndex = br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    br.image = im; br.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &br);
}
void hw_present_opts(float rcas, int bloom) { pr_rcas = rcas; pr_bloom = bloom; }
/* One presentation pass: full-target triangle, scissored to (x, y, w, h). */
static void ppass(VkImageView view, int tw, int th, int fmtsel, int clear, int x, int y, int w, int h, VkDescriptorSet s0, VkDescriptorSet s1,
                  const float *src, const float *dst, int o0, int o1, int o2, int o3, float par) {
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = (VkRect2D){ { 0, 0 }, { (uint32_t)tw, (uint32_t)th } }; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > tw ? tw : x + w, y1 = y + h > th ? th : y + h;
    if (s0 && x1 > x0 && y1 > y0) {
        VkViewport vp = { 0, 0, (float)tw, (float)th, 0.0f, 1.0f };
        VkRect2D sc = { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
        vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline((15u << 3) | (1u << 11) | ((uint32_t)fmtsel << 12)));
        VkDescriptorSet sets[2] = { s0, s1 ? s1 : dummy.set };
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 2, sets, 0, 0);
        struct { float src[4], dst[4]; int32_t opt[4]; float par[4]; } pp;
        memcpy(pp.src, src, 16); memcpy(pp.dst, dst, 16); pp.opt[0] = o0; pp.opt[1] = o1; pp.opt[2] = o2; pp.opt[3] = o3;
        pp.par[0] = par; pp.par[1] = pp.par[2] = pp.par[3] = 0;
        PushC pc; memset(&pc, 0, sizeof pc); memcpy(&pc, &pp, sizeof pp);
        vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cb);
}

/* ---- FSR 3.1 temporal upscaling (--filter fsr3) --------------------------------
 * AMD's FidelityFX API, loaded at run time from the signed DLLs beside the
 * program (amd_fidelityfx_loader_vk.dll, which loads
 * amd_fidelityfx_upscaler_vk.dll): Windows only, AMD publishes no Linux
 * build. Without the DLLs, or if the context cannot be made, hw_fsr3_init /
 * hw_fsr3_begin return 0 after one log line and the caller presents with
 * FSR 1.0 as before.
 *
 * One upscaled picture (the caller, gs.c, draws; this layer owns the images):
 *   hw_fsr3_begin    context for these sizes, next sub-pixel jitter
 *   (caller draws the jittered 3D scene into a colour target at the internal
 *    resolution, its motion vectors / depth / reactive values into a
 *    motion-vector target, and the 2D layer into two targets of its own scale,
 *    once over black and once over white)
 *   hw_fsr3_upscale  inputs out of those targets, ffxDispatch, then the 2D
 *                    layer over the result: picture * (W - B) + B, into one of
 *                    two kept pictures at output size ("slots")
 *   hw_fsr3_show     the next hw_present / hw_present_pixels shows that slot
 *                    instead of scaling the render target (colour controls
 *                    and bloom as usual; no RCAS: the upscaler has sharpened).
 * All of it is used by one thread: the presenter with --fps, else the game thread. */
#include "ffx_min.h"
#include "gs_hw_fsr3.h"
static int fsr_ok, fsr_debug, fsr_view;
static PfnFfxCreateContext ffx_create; static PfnFfxDestroyContext ffx_destroy; static PfnFfxQuery ffx_query; static PfnFfxDispatch ffx_dispatch;
static ffxContext fsr_ctx; static int fsr_rw, fsr_rh, fsr_dw, fsr_dh, fsr_phase, fsr_index, fsr_fresh; static float fsr_jx, fsr_jy;
static uint64_t fsr_pin; static int fsr_pinned;
static Img f_col, f_mv, f_depth, f_react, f_out, f_comp[2];
static TL int pr_fsr_slot = -1;
static void img_new(Img *im, int w, int h, VkFormat fmt, VkImageUsageFlags extra, int lin) {      /* colour image of any format, sampled */
    memset(im, 0, sizeof *im);
    im->w = w; im->h = h;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt;
    ci.extent = (VkExtent3D){ (uint32_t)w, (uint32_t)h, 1 }; ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | extra;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(dev, &ci, 0, &im->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECK(vkAllocateMemory(dev, &ai, 0, &im->mem));
    CHECK(vkBindImageMemory(dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = im->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    CHECK(vkCreateImageView(dev, &vi, 0, &im->view));
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dpool, 1, &dsl };
    pthread_mutex_lock(&dmx);
    VkResult ar_ = vkAllocateDescriptorSets(dev, &da, &im->set);
    pthread_mutex_unlock(&dmx);
    CHECK(ar_);
    VkDescriptorImageInfo ii = { sampler, im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, im->set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, 0, 0 };
    vkUpdateDescriptorSets(dev, 1, &wd, 0, 0);
    if (lin) img_add_lin(im);
}
void hw_fsr3_want(void) { fsr_want = 1; }
#ifdef _WIN32
static void fsr_message(uint32_t type, const wchar_t *msg) {
    char b[512]; int n = 0;
    for (; msg && msg[n] && n < 511; n++) b[n] = msg[n] < 128 ? (char)msg[n] : '?';
    b[n] = 0;
    fprintf(stderr, "[gs_hw] FidelityFX %s: %s\n", type == 0 ? "error" : "warning", b);
}
#endif
int hw_fsr3_init(void) {
    if (!fsr_want || !dev) return 0;
#ifdef _WIN32
    wchar_t path[1024]; DWORD n = GetModuleFileNameW(0, path, 900);
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    static const wchar_t name[] = L"amd_fidelityfx_loader_vk.dll";
    memcpy(path + n, name, sizeof name);
    HMODULE m = LoadLibraryExW(path, 0, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {                                    /* not beside the program: the folder named by ROR_APP_DIR (the launcher's own) */
        const wchar_t *app = _wgetenv(L"ROR_APP_DIR"); size_t al = app ? wcslen(app) : 0;
        if (al > 0 && al < 900) {
            memcpy(path, app, al * sizeof(wchar_t));
            if (path[al - 1] != L'\\' && path[al - 1] != L'/') path[al++] = L'\\';
            memcpy(path + al, name, sizeof name);
            m = LoadLibraryExW(path, 0, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
    }
    if (!m) { fprintf(stderr, "[gs_hw] FSR 3.1: amd_fidelityfx_loader_vk.dll could not be loaded from the program's folder or %%ROR_APP_DIR%% (error %lu): using FSR 1.0\n", (unsigned long)GetLastError()); return 0; }
    ffx_create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext"); ffx_destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    ffx_query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery"); ffx_dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    if (!ffx_create || !ffx_destroy || !ffx_query || !ffx_dispatch) { fprintf(stderr, "[gs_hw] FSR 3.1: amd_fidelityfx_loader_vk.dll does not export the FidelityFX API: using FSR 1.0\n"); return 0; }
    fsr_debug = getenv("ROR_FSR3_DEBUG") ? atoi(getenv("ROR_FSR3_DEBUG")) : 0;        /* 1: the upscaler checks its inputs and reports; 2: plus its debug view */
    fsr_view = getenv("ROR_FSR3_VIEW") ? atoi(getenv("ROR_FSR3_VIEW")) : 0;           /* show an input instead of the result: 1 colour, 2 motion, 3 depth, 4 reactive */
    {   /* the upscaler versions the DLL (and, on Radeon, the driver) offers; ROR_FSR_VERSION=text pins the first whose name contains it */
        VkDeviceContext dc = { dev, phys, vkGetDeviceProcAddr };
        uint64_t count = 0, ids[32]; const char *names[32]; char list[512]; int o = 0; const char *want = getenv("ROR_FSR_VERSION");
        struct ffxQueryDescGetVersions q = { { FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, 0 }, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &dc, &count, 0, 0 };
        if (ffx_query(0, &q.header) == FFX_API_RETURN_OK && count > 0) {
            if (count > 32) count = 32;
            q.versionIds = ids; q.versionNames = names;
            if (ffx_query(0, &q.header) == FFX_API_RETURN_OK) {
                list[0] = 0;
                for (uint64_t i = 0; i < count; i++) {
                    o += snprintf(list + o, sizeof list - (size_t)o, "%s%s", i ? ", " : "", names[i] ? names[i] : "?");
                    if (o > 480) break;
                    if (want && !fsr_pinned && names[i] && strstr(names[i], want)) { fsr_pin = ids[i]; fsr_pinned = 1; }
                }
                fprintf(stderr, "[gs_hw] FidelityFX upscaler versions available: %s%s\n", list, want ? (fsr_pinned ? " (one pinned by ROR_FSR_VERSION)" : " (ROR_FSR_VERSION matches none)") : "");
            }
        }
    }
    fsr_ok = 1;
    return 1;
#else
    fprintf(stderr, "[gs_hw] FSR 3.1 needs AMD's FidelityFX DLL, which exists for Windows only: using FSR 1.0\n");
    return 0;
#endif
}
static void fsr_free_images(void) {
    Img *all[7] = { &f_col, &f_mv, &f_depth, &f_react, &f_out, &f_comp[0], &f_comp[1] };
    for (int i = 0; i < 7; i++) if (all[i]->img) img_destroy(all[i]);
}
int hw_fsr3_begin(int rw, int rh, int dw, int dh, float *jx, float *jy) {
    if (!fsr_ok || rw <= 0 || rh <= 0 || dw <= 0 || dh <= 0) return 0;
    if (!fsr_ctx || rw != fsr_rw || rh != fsr_rh || dw != fsr_dw || dh != fsr_dh) {
        hw_flush();                                    /* nothing of this thread's is pending; no other thread uses these objects */
        if (fsr_ctx) { ffx_destroy(&fsr_ctx, 0); fsr_ctx = 0; }
        fsr_free_images();
        struct ffxCreateBackendVKDesc be = { { FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, 0 }, { dev, phys, vkGetDeviceProcAddr } };
        struct ffxCreateContextDescUpscaleVersion ver = { { FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &be.header }, FFX_UPSCALER_VERSION };
        struct ffxOverrideVersion pin = { { FFX_API_DESC_TYPE_OVERRIDE_VERSION, &ver.header }, fsr_pin };
        struct ffxCreateContextDescUpscale cu; memset(&cu, 0, sizeof cu);
        cu.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; cu.header.pNext = fsr_pinned ? &pin.header : &ver.header;
        /* depth: 1 at the near plane, 0 at the far plane (see the motion-vector pass); colour: the GS's 8-bit gamma-encoded picture */
        cu.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE |
                   (fsr_debug ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0) | (fsr_debug > 1 ? FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION : 0);
        cu.maxRenderSize.width = (uint32_t)rw; cu.maxRenderSize.height = (uint32_t)rh; cu.maxUpscaleSize.width = (uint32_t)dw; cu.maxUpscaleSize.height = (uint32_t)dh;
#ifdef _WIN32
        cu.fpMessage = fsr_message;
#endif
        ffxReturnCode_t rc = ffx_create(&fsr_ctx, &cu.header, 0);
        if (rc != FFX_API_RETURN_OK || !fsr_ctx) {
            fprintf(stderr, "[gs_hw] FSR 3.1: the upscaler context for %dx%d -> %dx%d could not be created (FidelityFX error %u): using FSR 1.0\n", rw, rh, dw, dh, (unsigned)rc);
            fsr_ctx = 0; fsr_ok = 0; return 0;
        }
        struct ffxQueryGetProviderVersion pv = { { FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, 0 }, 0, 0 };
        ffx_query(&fsr_ctx, &pv.header);
        int32_t phase = 0;
        struct ffxQueryDescUpscaleGetJitterPhaseCount pq = { { FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT, 0 }, (uint32_t)rw, (uint32_t)dw, &phase };
        if (ffx_query(&fsr_ctx, &pq.header) != FFX_API_RETURN_OK || phase < 1) phase = 8;
        fsr_phase = phase; fsr_index = 0; fsr_fresh = 1;
        fsr_rw = rw; fsr_rh = rh; fsr_dw = dw; fsr_dh = dh;
        fprintf(stderr, "[gs_hw] temporal upscaler: %s, %dx%d -> %dx%d, %d jitter phases\n", pv.versionName ? pv.versionName : "FidelityFX (version unknown)", rw, rh, dw, dh, phase);
        img_new(&f_col, rw, rh, VK_FORMAT_R8G8B8A8_UNORM, 0, 1);
        img_new(&f_mv, rw, rh, VK_FORMAT_R16G16_SFLOAT, 0, 0);
        img_new(&f_depth, rw, rh, VK_FORMAT_R32_SFLOAT, 0, 0);
        img_new(&f_react, rw, rh, VK_FORMAT_R8_UNORM, 0, 0);
        img_new(&f_out, dw, dh, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, 0);
        img_new(&f_comp[0], dw, dh, VK_FORMAT_R16G16B16A16_SFLOAT, 0, 0); img_new(&f_comp[1], dw, dh, VK_FORMAT_R16G16B16A16_SFLOAT, 0, 0);
    }
    float qx = 0, qy = 0;
    struct ffxQueryDescUpscaleGetJitterOffset jq = { { FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET, 0 }, fsr_index++, fsr_phase, &qx, &qy };
    ffx_query(&fsr_ctx, &jq.header);
    /* The offset handed to the upscaler is minus the sequence value, and the picture is drawn moved by that offset
     * (what AMD's sample does through its projection matrix: clip-space shift (-2x/w, +2y/h), y up). */
    fsr_jx = -qx; fsr_jy = -qy;
    if (getenv("ROR_FSR3_NOJITTER")) fsr_jx = fsr_jy = 0;
    *jx = fsr_jx; *jy = fsr_jy;
    {   static int flip = -1; if (flip < 0) flip = getenv("ROR_FSR3_JFLIP") ? atoi(getenv("ROR_FSR3_JFLIP")) : 0;      /* test: tell the upscaler the opposite x (1) / y (2) offset */
        if (flip & 1) fsr_jx = -fsr_jx; if (flip & 2) fsr_jy = -fsr_jy; }
    return 1;
}
/* One fsr3.frag pass over the whole target. load: keep the target (additive pass). */
static void fpass(VkImageView view, int tw, int th, int fmtsel, int add, VkDescriptorSet s0, VkDescriptorSet s1, const float *src, const float *dst, int ox, int ow, float par) {
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = add ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = (VkRect2D){ { 0, 0 }, { (uint32_t)tw, (uint32_t)th } }; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)tw, (float)th, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { (uint32_t)tw, (uint32_t)th } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline((add ? (uint32_t)HWB_ADD | (1u << 2) : 0u) | (15u << 3) | (1u << 11) | ((uint32_t)fmtsel << 12) | (1u << 16)));
    if (add) { float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; vkCmdSetBlendConstants(cb, one); }
    VkDescriptorSet sets[2] = { s0, s1 ? s1 : dummy.set };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 2, sets, 0, 0);
    struct { float src[4], dst[4]; int32_t opt[4]; float par[4]; } pp;
    memcpy(pp.src, src, 16); memcpy(pp.dst, dst, 16); pp.opt[0] = ox; pp.opt[1] = pp.opt[2] = 0; pp.opt[3] = ow;
    pp.par[0] = par; pp.par[1] = pp.par[2] = pp.par[3] = 0;
    PushC pc; memset(&pc, 0, sizeof pc); memcpy(&pc, &pp, sizeof pp);
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}
static struct FfxApiResource fsr_res(Img *im, uint32_t fmt, uint32_t usage) {
    struct FfxApiResource r; memset(&r, 0, sizeof r);
    r.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    if (!im) return r;
    r.resource = (void *)im->img;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D; r.description.format = fmt; r.description.width = (uint32_t)im->w; r.description.height = (uint32_t)im->h;
    r.description.depth = 1; r.description.mipCount = 1; r.description.flags = FFX_API_RESOURCE_FLAGS_NONE; r.description.usage = usage;
    return r;
}
int hw_fsr3_upscale(const HwFsr3 *p) {
    if (!fsr_ok || !fsr_ctx || p->slot < 0 || p->slot > 1) return 0;
    Rt *c = &rts[p->color], *m = &rts[p->mv];
    int rw = p->sw * N, rh = p->sh * N, ox = p->sx * N, oy = p->sy * N;
    if (rw != fsr_rw || rh != fsr_rh || ox + rw > c->col.w || oy + rh > c->col.h || ox + rw > m->col.w || oy + rh > m->col.h) {
        fprintf(stderr, "[gs_hw] FSR 3.1: picture rectangle %d,%d %dx%d does not fit its targets (%dx%d, %dx%d; context %dx%d): using FSR 1.0\n", ox, oy, rw, rh, c->col.w, c->col.h, m->col.w, m->col.h, fsr_rw, fsr_rh);
        return 0;
    }
    emit_batch(); end_rendering(); begin_cb();
    float off[4] = { (float)ox, (float)oy, 0, 0 }, zero[4] = { 0, 0, 0, 0 }, full[4] = { 0, 0, (float)fsr_dw, (float)fsr_dh };
    /* the upscaler's inputs, each exactly render size: colour (copy of the picture rectangle), motion, depth, reactive mask */
    to_layout(&c->col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); to_layout(&f_col, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { ox, oy, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { (uint32_t)rw, (uint32_t)rh, 1 } };
    vkCmdCopyImage(cb, c->col.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f_col.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    to_layout(&m->col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_layout(&f_mv, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); to_layout(&f_depth, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); to_layout(&f_react, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fpass(f_mv.view, rw, rh, 6, 0, m->col.set, VK_NULL_HANDLE, off, zero, 0, 0, 0.0f);
    fpass(f_depth.view, rw, rh, 4, 0, m->col.set, VK_NULL_HANDLE, off, zero, 1, 0, 0.0f);
    fpass(f_react.view, rw, rh, 5, 0, m->col.set, VK_NULL_HANDLE, off, zero, 2, 0, 0.0f);
    to_layout(&f_col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); to_layout(&f_mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_layout(&f_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); to_layout(&f_react, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_layout(&f_out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Img *shown = &f_out; int view = fsr_view;
    if (view < 1 || view > 4) {
        struct ffxDispatchDescUpscale d; memset(&d, 0, sizeof d);
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = (void *)cb;
        d.color = fsr_res(&f_col, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.depth = fsr_res(&f_depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.motionVectors = fsr_res(&f_mv, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.exposure = fsr_res(0, 0, 0);
        d.reactive = p->no_reactive ? fsr_res(0, 0, 0) : fsr_res(&f_react, FFX_API_SURFACE_FORMAT_R8_UNORM, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.transparencyAndComposition = fsr_res(0, 0, 0);
        d.output = fsr_res(&f_out, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_RESOURCE_USAGE_UAV);
        d.jitterOffset.x = fsr_jx; d.jitterOffset.y = fsr_jy;
        d.motionVectorScale.x = 1.0f; d.motionVectorScale.y = 1.0f;         /* the motion image is in render pixels */
        d.renderSize.width = (uint32_t)rw; d.renderSize.height = (uint32_t)rh; d.upscaleSize.width = (uint32_t)fsr_dw; d.upscaleSize.height = (uint32_t)fsr_dh;
        /* --sharpness S: the RCAS constant handed over for FSR 1.0 is exp2(-(100 - S) / 50); the upscaler's own RCAS uses exp2(-2 (1 - sharpness)) */
        float sh = pr_rcas > 0.0f ? 1.0f + 0.5f * log2f(pr_rcas) : 0.0f;
        d.enableSharpening = sh > 0.004f; d.sharpness = sh < 0.0f ? 0.0f : sh > 1.0f ? 1.0f : sh;
        d.frameTimeDelta = p->dt_ms < 1.0f ? 1.0f : p->dt_ms; d.preExposure = 1.0f;
        d.reset = p->reset || fsr_fresh;
        d.cameraNear = p->cam_near; d.cameraFar = p->cam_far; d.cameraFovAngleVertical = p->cam_fovy; d.viewSpaceToMetersFactor = p->unit_m;
        d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB | (fsr_debug > 1 ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0);
        ffxReturnCode_t rc = ffx_dispatch(&fsr_ctx, &d.header);
        if (rc != FFX_API_RETURN_OK) {
            fprintf(stderr, "[gs_hw] FSR 3.1: upscaling failed (FidelityFX error %u): using FSR 1.0 from here on\n", (unsigned)rc);
            fsr_ok = 0; rendering = 0; cur_rt = cur_ds = -1;
            return 0;
        }
        fsr_fresh = 0;
        rendering = 0; cur_rt = cur_ds = -1;         /* the upscaler bound its own pipelines */
    }
    /* the 2D layer over the upscaled picture */
    Img *out = &f_comp[p->slot];
    to_layout(out, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (view >= 1 && view <= 4) {
        float rr[4] = { 0, 0, (float)rw, (float)rh };
        if (view == 1) fpass(out->view, fsr_dw, fsr_dh, 2, 0, f_col.set_lin, VK_NULL_HANDLE, rr, full, 0, 4, 0.0f);
        else { float mr[4] = { (float)ox, (float)oy, (float)rw, (float)rh }; fpass(out->view, fsr_dw, fsr_dh, 2, 0, m->col.set_lin, VK_NULL_HANDLE, mr, full, view - 1, 4, 0.125f); }
    } else if (p->ovl_b >= 0 && p->ovl_w >= 0) {
        Rt *b = &rts[p->ovl_b], *w = &rts[p->ovl_w];
        float orr[4] = { (float)(p->sx * b->n), (float)(p->sy * b->n), (float)(p->sw * b->n), (float)(p->sh * b->n) };
        to_layout(&b->col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); to_layout(&w->col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        fpass(out->view, fsr_dw, fsr_dh, 2, 0, shown->set, w->col.set_lin, orr, full, 0, 1, 0.0f);
        fpass(out->view, fsr_dw, fsr_dh, 2, 1, shown->set, b->col.set_lin, orr, full, 0, 2, 0.0f);
    } else fpass(out->view, fsr_dw, fsr_dh, 2, 0, shown->set, VK_NULL_HANDLE, full, full, 0, 3, 0.0f);
    to_layout(out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}
void hw_fsr3_show(int slot) { pr_fsr_slot = slot; }
static void stage_img(Img *im, int w, int h, int f16) {
    if (im->img && (im->w != w || im->h != h)) { hw_flush(); img_destroy(im); }
    if (!im->img) { img_create(im, w, h, f16 ? 2 : 0, 1); img_add_lin(im); }
}
/* The picture into a target (swapchain image or off-screen image; the caller
 * has put it in COLOR_ATTACHMENT layout). filter: 0 nearest, 1 bilinear, 2 FSR
 * (EASU when the internal picture is smaller than the rectangle, an area
 * filter when it is larger; RCAS after either). With FSR or bloom the
 * picture goes through the stage images, otherwise it is one pass. */
static void present_draw(Rt *r, VkImageView view, int tw, int th, int fmtsel, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int filter, int sat64) {
    float src[4] = { (float)(sx * N), (float)(sy * N), (float)(sw * N), (float)(sh * N) }, dst[4] = { (float)dx, (float)dy, (float)dw, (float)dh };
    /* FSR 3.1: a finished picture of exactly this size is waiting (hw_fsr3_show); otherwise filter 3 means FSR 1.0 */
    Img *up = 0;
    if (pr_fsr_slot >= 0) { if (r && filter == 3 && fsr_ok && f_comp[pr_fsr_slot].img && f_comp[pr_fsr_slot].w == dw && f_comp[pr_fsr_slot].h == dh) up = &f_comp[pr_fsr_slot]; pr_fsr_slot = -1; }
    if (filter == 3) filter = 2;
    int bloom = pr_bloom > 0, chain = r && dw > 0 && dh > 0 && (filter == 2 || bloom);
    if (!chain) {
        ppass(view, tw, th, fmtsel, 1, dx, dy, dw, dh, r ? (filter ? r->col.set_lin : r->col.set) : VK_NULL_HANDLE, lut_on ? lut_img.set : VK_NULL_HANDLE,
              src, dst, filter != 0, lut_on, sat64, 0, 0.0f);
        return;
    }
    float full[4] = { 0, 0, (float)dw, (float)dh };
    stage_img(&st_a, dw, dh, 1); stage_img(&st_b, dw, dh, 0);
    int mode = filter == 2 ? (sw * N < dw ? 2 : sw * N > dw ? 3 : 0) : filter;
    if (up) {                       /* stage B <- the upscaled picture: colour controls only */
        to_layout(&st_b, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ppass(st_b.view, dw, dh, 1, 0, 0, 0, dw, dh, up->set, lut_on ? lut_img.set : VK_NULL_HANDLE, full, full, 0, lut_on, sat64, 2, 0.0f);
    } else {
    to_layout(&st_a, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ppass(st_a.view, dw, dh, 2, 0, 0, 0, dw, dh, mode == 0 || mode == 2 ? r->col.set : r->col.set_lin, VK_NULL_HANDLE, src, full, mode, 0, 64, 1, 0.0f);
    to_layout(&st_a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); to_layout(&st_b, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ppass(st_b.view, dw, dh, 1, 0, 0, 0, dw, dh, st_a.set, lut_on ? lut_img.set : VK_NULL_HANDLE, full, full, 0, lut_on, sat64, 2, filter == 2 ? pr_rcas : 0.0f);
    }
    to_layout(&st_b, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (bloom) {                    /* post.c: highlights of quarter-size cells, three box blurs of 2% / 2.7% of the glow size */
        int gw = sw / 4 > 0 ? sw / 4 : 1, gh = sh / 4 > 0 ? sh / 4 : 1, rx = (gw * 2 + 50) / 100, ry = (gh * 27 + 500) / 1000;
        float g[4] = { 0, 0, (float)gw, (float)gh };
        if (rx < 1) rx = 1; if (ry < 1) ry = 1;
        stage_img(&gl_a, gw, gh, 0); stage_img(&gl_b, gw, gh, 0);
        to_layout(&gl_a, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ppass(gl_a.view, gw, gh, 1, 0, 0, 0, gw, gh, r->col.set_lin, lut_on ? lut_img.set : VK_NULL_HANDLE, src, g, 0, lut_on, sat64, 4, (float)pr_bloom);
        for (int k = 0; k < 3; k++) {
            to_layout(&gl_a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); to_layout(&gl_b, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            ppass(gl_b.view, gw, gh, 1, 0, 0, 0, gw, gh, gl_a.set, VK_NULL_HANDLE, g, g, 0, 0, 64, 5, (float)rx);
            to_layout(&gl_b, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); to_layout(&gl_a, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            ppass(gl_a.view, gw, gh, 1, 0, 0, 0, gw, gh, gl_b.set, VK_NULL_HANDLE, g, g, 0, 0, 64, 6, (float)ry);
        }
        to_layout(&gl_a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    ppass(view, tw, th, fmtsel, 1, dx, dy, dw, dh, st_b.set, bloom ? gl_a.set_lin : VK_NULL_HANDLE, full, dst, 0, bloom, 64, 3, 0.0f);
}
const uint32_t *hw_present_pixels(int rt, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int ww, int wh, int linear, int sat64) {
    static TL Img off;
    if (off.img && (off.w != ww || off.h != wh)) { hw_flush(); img_destroy(&off); }
    if (!off.img) img_create(&off, ww, wh, 0, 0);
    size_t bytes = (size_t)ww * (size_t)wh * 4;
    if (roff + bytes > rcap) die("read-back buffer full", 0);
    emit_batch(); end_rendering(); begin_cb();
    Rt *r = rt >= 0 ? &rts[rt] : 0;
    if (r) to_layout(&r->col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_layout(&off, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    present_draw(r, off.view, ww, wh, 1, sx, sy, sw, sh, dx, dy, dw, dh, linear, sat64);
    begin_cb();
    to_layout(&off, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bc = { roff, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { (uint32_t)ww, (uint32_t)wh, 1 } };
    vkCmdCopyImageToBuffer(cb, off.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &bc);
    size_t at = roff;
    hw_flush();
    return (const uint32_t *)(rmap + at);
}
void hw_present(int rt, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int ww, int wh, int linear, int sat64) {
    if (!surf) return;
    if ((!swp || (int)swext.width != ww || (int)swext.height != wh) && ww > 0 && wh > 0) {
        hw_flush();
        pthread_mutex_lock(&qm[0]); pthread_mutex_lock(&qm[1]); vkDeviceWaitIdle(dev); int okc = swap_create(ww, wh); pthread_mutex_unlock(&qm[1]); pthread_mutex_unlock(&qm[0]);
        if (!okc && !swp) return;
    }
    if (!swp) return;
    uint32_t idx = 0;
    VkResult ar = vkAcquireNextImageKHR(dev, swp, ~0ull, sem_acq, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        hw_flush();
        pthread_mutex_lock(&qm[0]); pthread_mutex_lock(&qm[1]); vkDeviceWaitIdle(dev); int okc = swap_create(ww, wh); pthread_mutex_unlock(&qm[1]); pthread_mutex_unlock(&qm[0]);
        if (!okc) return;
        ar = vkAcquireNextImageKHR(dev, swp, ~0ull, sem_acq, VK_NULL_HANDLE, &idx);
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return;
    emit_batch(); end_rendering(); begin_cb();
    Rt *r = rt >= 0 ? &rts[rt] : 0;
    if (r) to_layout(&r->col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sw_barrier(swimg[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    present_draw(r, swview[idx], (int)swext.width, (int)swext.height, 0, sx, sy, sw, sh, dx, dy, dw, dh, linear, sat64);
    int grabbed = 0;
    if (grab_want && sw_can_grab && roff + (size_t)swext.width * swext.height * 4 <= rcap) {       /* copy of exactly what is presented */
        sw_barrier(swimg[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bc = { roff, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { swext.width, swext.height, 1 } };
        vkCmdCopyImageToBuffer(cb, swimg[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &bc);
        sw_barrier(swimg[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        grab_at = roff; grab_w = (int)swext.width; grab_h = (int)swext.height; grabbed = 1;
    } else sw_barrier(swimg[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    grab_want = grabbed ? 2 : 0;
    submit_cb(sem_acq, sem_done);
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &sem_done, 1, &swp, &idx, 0 };
    pthread_mutex_lock(&qm[qi]);
    vkQueuePresentKHR(queues[qi], &pi);
    pthread_mutex_unlock(&qm[qi]);
}
static void need_staging(size_t bytes) { if (soff + bytes > st_cap) hw_flush(); if (bytes > st_cap) die("staging too small", 0); }

int hw_rt_create(int w, int h) {
    if (nrt == MAX_RT) die("too many render targets", 0);
    Rt *r = &rts[nrt];
    memset(r, 0, sizeof *r);
    r->w = w; r->h = h; r->n = N;
    img_create(&r->col, w * N, h * N, 0, 1); img_create(&r->copy, w * N, h * N, 0, 1); img_create(&r->up, w, h, 0, 1);
    img_add_lin(&r->col);
    r->ver = 1;
    return nrt++;
}
int hw_ds_create(int w, int h) {
    if (nds == MAX_RT) die("too many depth buffers", 0);
    img_create(&dss[nds], w * N, h * N, 1, 1);
    hw_ds_clear(nds);
    return nds++;
}
/* FSR 3.1: a target at its own scale n (the 2D layer), or (mv) a motion-vector
 * target for the MV variant of the GS shaders: RGBA32F at the renderer's scale. */
static void img_new(Img *im, int w, int h, VkFormat fmt, VkImageUsageFlags extra, int lin);
int hw_rt_create_n(int w, int h, int n, int mv) {
    if (nrt == MAX_RT) die("too many render targets", 0);
    Rt *r = &rts[nrt];
    memset(r, 0, sizeof *r);
    r->w = w; r->h = h; r->n = mv ? N : n; r->mv = mv;
    if (mv) img_new(&r->col, w * N, h * N, VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1);
    else { img_create(&r->col, w * n, h * n, 0, 1); img_create(&r->copy, w * n, h * n, 0, 1); img_add_lin(&r->col); }
    r->ver = 1;
    return nrt++;
}
int hw_ds_create_n(int w, int h, int n) {
    if (nds == MAX_RT) die("too many depth buffers", 0);
    img_create(&dss[nds], w * n, h * n, 1, 1);
    hw_ds_clear(nds);
    return nds++;
}
void hw_rt_clear(int rt, float cr, float cg, float cb_, float ca) {
    Rt *r = &rts[rt];
    emit_batch(); end_rendering(); begin_cb();
    to_layout(&r->col, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue cv = { { cr, cg, cb_, ca } };
    VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, r->col.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &sr);
    r->ver++;
}
void hw_mv_params(float zfar, float zinv) { emit_batch(); mv_zfar = zfar; mv_zinv = zinv; }
void hw_ds_clear(int ds) {
    emit_batch(); end_rendering(); begin_cb();
    to_layout(&dss[ds], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue cv = { 0.0f, 0 };
    VkImageSubresourceRange sr = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    vkCmdClearDepthStencilImage(cb, dss[ds].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &sr);
}
static void begin_rendering(int rt, int ds) {
    if (rendering && cur_rt == rt && cur_ds == ds) return;
    end_rendering(); begin_cb();
    to_layout(&rts[rt].col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = rts[rt].col.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = ca;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = (VkRect2D){ { 0, 0 }, { (uint32_t)rts[rt].col.w, (uint32_t)rts[rt].col.h } };
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    if (ds >= 0) {
        to_layout(&dss[ds], VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        da.imageView = dss[ds].view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ri.pDepthAttachment = &da;
    }
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)rts[rt].col.w, (float)rts[rt].col.h, 0.0f, 1.0f };
    vkCmdSetViewport(cb, 0, 1, &vp);
    rendering = 1; cur_rt = rt; cur_ds = ds;
}
static void upload_img(Img *im, int x, int y, int w, int h, const uint32_t *rgba) {      /* leaves the image in TRANSFER_DST */
    size_t bytes = (size_t)w * (size_t)h * 4;
    need_staging(bytes);
    end_rendering(); begin_cb();
    memcpy(smap + soff, rgba, bytes);
    to_layout(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bc = { soff, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { x, y, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
    vkCmdCopyBufferToImage(cb, sbuf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    soff += (bytes + 15) & ~(size_t)15;
}
void hw_rt_upload(int rt, int x, int y, int w, int h, const uint32_t *rgba) {
    Rt *r = &rts[rt];
    emit_batch();
    upload_img(&r->up, x, y, w, h, rgba);
    to_layout(&r->up, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_layout(&r->col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = r->col.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = (VkRect2D){ { 0, 0 }, { (uint32_t)r->col.w, (uint32_t)r->col.h } };
    ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)r->col.w, (float)r->col.h, 0.0f, 1.0f };
    VkRect2D sc = { { x * N, y * N }, { (uint32_t)(w * N), (uint32_t)(h * N) } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, blit_pipe);
    VkDescriptorSet sets[2] = { r->up.set, dummy.set };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 2, sets, 0, 0);
    PushC pc; memset(&pc, 0, sizeof pc);
    pc.flags = 0; pc.aref = 0;
    /* blit.frag's block: vec4 rt; vec4 src (offset 16): x, y unused offset 0, scale */
    float src[4] = { 0, 0, (float)N, 0 };
    memcpy((char *)&pc + 16, src, 16);
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    rendering = 0; cur_rt = -1;
    r->ver++;
}
static size_t queue_native_read(int rt, int x, int y, int w, int h) {       /* returns offset in the read-back buffer */
    Rt *r = &rts[rt];
    emit_batch(); end_rendering(); begin_cb();
    size_t bytes = (size_t)w * (size_t)h * 4;
    if (roff + bytes > rcap) die("read-back buffer full", 0);
    to_layout(&r->col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Img *src = &r->col; int sx = x, sy = y;
    if (N > 1) {            /* reduce to native size first: the sample of each block is its first sub-pixel (the GS sample point) */
        to_layout(&r->up, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        /* a blit with NEAREST picks the texel under the centre of each destination pixel: shift the source by half a block */
        VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { x * N - N / 2, y * N - N / 2, 0 }, { (x + w) * N - N / 2, (y + h) * N - N / 2, 1 } },
                           { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { x, y, 0 }, { x + w, y + h, 1 } } };
        if (bl.srcOffsets[0].x < 0 || bl.srcOffsets[0].y < 0) {            /* cannot shift at the border: plain centre sampling */
            bl.srcOffsets[0] = (VkOffset3D){ x * N, y * N, 0 }; bl.srcOffsets[1] = (VkOffset3D){ (x + w) * N, (y + h) * N, 1 };
        }
        vkCmdBlitImage(cb, r->col.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->up.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        to_layout(&r->up, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        src = &r->up;
    }
    VkBufferImageCopy bc = { roff, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { sx, sy, 0 }, { (uint32_t)w, (uint32_t)h, 1 } };
    vkCmdCopyImageToBuffer(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &bc);
    size_t at = roff; roff += (bytes + 15) & ~(size_t)15;
    return at;
}
void hw_rt_download(int rt, int x, int y, int w, int h, uint32_t *rgba) {
    size_t save = roff;
    size_t at = queue_native_read(rt, x, y, w, h);
    hw_flush();
    memcpy(rgba, rmap + at, (size_t)w * (size_t)h * 4);
    roff = save;
}
int hw_rt_copy_async(int rt, int x, int y, int w, int h) {
    if (ntickets == 256) return -1;
    tickets[ntickets].off = queue_native_read(rt, x, y, w, h); tickets[ntickets].w = w; tickets[ntickets].h = h;
    return ntickets++;
}
void hw_ticket_read(int ticket, uint32_t *rgba) {
    memcpy(rgba, rmap + tickets[ticket].off, (size_t)tickets[ticket].w * (size_t)tickets[ticket].h * 4);
    if (ticket == ntickets - 1) {           /* last one handed out has been collected: recycle the buffer */
        ntickets = 0; roff = 0;
    }
}
const uint32_t *hw_rt_read_scaled(int rt, int x, int y, int w, int h) {
    Rt *r = &rts[rt];
    emit_batch(); end_rendering(); begin_cb();
    size_t bytes = (size_t)w * N * (size_t)h * N * 4;
    if (roff + bytes > rcap) die("read-back buffer full", 0);
    to_layout(&r->col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bc = { roff, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { x * N, y * N, 0 }, { (uint32_t)(w * N), (uint32_t)(h * N), 1 } };
    vkCmdCopyImageToBuffer(cb, r->col.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &bc);
    size_t at = roff;
    hw_flush();
    return (const uint32_t *)(rmap + at);
}
/* Rows [dy, dy + rows) of a target <- 16-bit depth memory seen as 32-bit
 * colour. table: 64x32 RGBA, per pixel of a 32-bit page the two depth pixels
 * (x0, y0, x1, y1 inside a 64x64 depth page) its low and high halves are;
 * page0: the rows' first page minus the depth buffer's first page; bw: buffer
 * width in pages. */
void hw_rt_rows_from_depth(int rt, int dy, int rows, int ds, const uint32_t *table, int page0, int bw) {
    static Img tab; static const uint32_t *tab_for;
    Rt *r = &rts[rt];
    emit_batch();
    if (tab_for != table) {
        if (!tab.img) img_create(&tab, 64, 32, 0, 1);
        upload_img(&tab, 0, 0, 64, 32, table);
        to_layout(&tab, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tab_for = table;
    }
    end_rendering(); begin_cb();
    to_layout(&dss[ds], VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    to_layout(&r->col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    float src[4] = { 0, 0, 0, 0 }, dst[4] = { 0, (float)(dy * N), (float)r->col.w, (float)(rows * N) };
    /* ppass with a LOAD of the rest of the target: do it by hand (ppass clears or discards) */
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = r->col.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = (VkRect2D){ { 0, 0 }, { (uint32_t)r->col.w, (uint32_t)r->col.h } }; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, (float)r->col.w, (float)r->col.h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, dy * N }, { (uint32_t)r->col.w, (uint32_t)(rows * N) } };
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline((15u << 3) | (1u << 11) | (1u << 12)));
    VkDescriptorSet sets[2] = { tab.set, dss[ds].set };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 2, sets, 0, 0);
    struct { float src[4], dst[4]; int32_t opt[4]; float par[4]; } pp;
    memcpy(pp.src, src, 16); memcpy(pp.dst, dst, 16); pp.opt[0] = N; pp.opt[1] = page0; pp.opt[2] = bw; pp.opt[3] = 7; memset(pp.par, 0, 16);
    PushC pc; memset(&pc, 0, sizeof pc); memcpy(&pc, &pp, sizeof pp);
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    r->ver++; cur_rt = -1;
}
void hw_rt_copy_rows(int src, int sy, int dst, int dy, int rows) {
    Rt *a = &rts[src], *b = &rts[dst];
    emit_batch(); end_rendering(); begin_cb();
    to_layout(&a->col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); to_layout(&b->col, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    int w = a->col.w < b->col.w ? a->col.w : b->col.w;
    VkImageCopy ic = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, sy * N, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, dy * N, 0 }, { (uint32_t)w, (uint32_t)(rows * N), 1 } };
    vkCmdCopyImage(cb, a->col.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->col.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    b->ver++; hw_stat_copies++;
}
int hw_rt_count(void) { return nrt; }
static void rt_copy_img(Rt *r, Img *from, Img *to) {
    emit_batch(); end_rendering(); begin_cb();
    to_layout(from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); to_layout(to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { (uint32_t)r->col.w, (uint32_t)r->col.h, 1 } };
    vkCmdCopyImage(cb, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
}
/* Copies of a target for frame interpolation: sv = as a frame found it, fin = as
 * the frame left it. Written by the game thread; then left in TRANSFER_SRC
 * layout and only read (hw_rt_load, any thread) until the slot comes round again. */
void hw_rt_save(int rt, int slot, int final) {
    Rt *r = &rts[rt]; Img *to = final ? &r->fin[slot] : &r->sv[slot];
    if (!to->img) img_create(to, r->col.w, r->col.h, 0, 0);
    rt_copy_img(r, &r->col, to);
    to_layout(to, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}
void hw_rt_load(int dst, int src, int slot, int final) {
    Rt *d = &rts[dst], *r = &rts[src]; Img *from = final ? &r->fin[slot] : &r->sv[slot];
    if (!from->img) return;
    rt_copy_img(d, from, &d->col);
    d->ver++;
}
void hw_rt_restore(int rt, int slot) { hw_rt_load(rt, rt, slot, 0); }
static void ds_copy(Img *from, Img *to) {
    emit_batch(); end_rendering(); begin_cb();
    to_layout(from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); to_layout(to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = { { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, { 0, 0, 0 }, { (uint32_t)from->w, (uint32_t)from->h, 1 } };
    vkCmdCopyImage(cb, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
}
void hw_ds_save(int ds, int slot) {
    Img *to = &dsv[ds][slot];
    if (!to->img) img_create(to, dss[ds].w, dss[ds].h, 1, 0);
    ds_copy(&dss[ds], to);
    to_layout(to, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}
void hw_ds_load(int dst, int src, int slot) {
    if (!dsv[src][slot].img) return;
    ds_copy(&dsv[src][slot], &dss[dst]);
}
/* Ask for a copy of the next presented swapchain image; afterwards
 * hw_grab_result gives it (pixels in swapchain byte order: *bgra says which). */
void hw_grab_request(void) { grab_want = 1; }
const uint32_t *hw_grab_result(int *w, int *h, int *bgra) {
    if (grab_want != 2) return 0;
    grab_want = 0; *w = grab_w; *h = grab_h; *bgra = swfmt == VK_FORMAT_B8G8R8A8_UNORM || swfmt == VK_FORMAT_B8G8R8A8_SRGB;
    return (const uint32_t *)(rmap + grab_at);
}
int hw_tex_create(int w, int h, const uint32_t *rgba) {
    int id = -1;
    for (int i = 0; i < MAX_TEX; i++) if (!tex_used[i]) { id = i; break; }
    if (id < 0) { hw_flush(); for (int i = 0; i < MAX_TEX; i++) if (!tex_used[i]) { id = i; break; } }
    if (id < 0) die("too many textures", 0);
    emit_batch();
    tex_used[id] = 1;
    img_create(&texs[id], w, h, 0, 1);
    upload_img(&texs[id], 0, 0, w, h, rgba);
    to_layout(&texs[id], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return id;
}
void hw_tex_destroy(int tex) { if (tex >= 0 && tex_used[tex] == 1) { tex_used[tex] = 2; tex_dead_gen[ntex_dead] = keep_gen; tex_dead[ntex_dead++] = tex; } }
void hw_keep_generation(void) { keep_gen++; }

/* ---- drawing ---------------------------------------------------------------- */
static void copy_rt(Rt *r, int x0, int y0, int x1, int y1) {        /* device rect [x0,x1) x [y0,y1): col -> copy */
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > r->col.w) x1 = r->col.w; if (y1 > r->col.h) y1 = r->col.h;
    if (x1 <= x0 || y1 <= y0) return;
    end_rendering();
    to_layout(&r->col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); to_layout(&r->copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { x0, y0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { x0, y0, 0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 1 } };
    vkCmdCopyImage(cb, r->col.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->copy.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    to_layout(&r->copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    hw_stat_copies++;
}
static void draw_range(const HwState *s, size_t first, int count, int passmode, int cmask, int zwrite, Img *tex, Img *dst) {
    Rt *r = &rts[s->rt];
    begin_rendering(s->rt, s->ds);
    const int N = r->n;                           /* the target's own scale (shadows the renderer's) */
    uint32_t key = (uint32_t)s->blend_mode | ((uint32_t)s->blend_const << 2) | ((uint32_t)cmask << 3) | ((uint32_t)s->ztest << 7) | ((uint32_t)zwrite << 9) | (r->mv ? 1u << 15 : 0u);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(key));
    VkDescriptorSet sets[2] = { tex ? tex->set : dummy.set, dst ? dst->set : dummy.set };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 2, sets, 0, 0);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &zero);
    int sx0 = s->sx0 * N, sy0 = s->sy0 * N, sx1 = (s->sx1 + 1) * N, sy1 = (s->sy1 + 1) * N;
    if (sx1 > r->col.w) sx1 = r->col.w; if (sy1 > r->col.h) sy1 = r->col.h;
    if (sx0 >= sx1 || sy0 >= sy1) return;
    VkRect2D sc = { { sx0, sy0 }, { (uint32_t)(sx1 - sx0), (uint32_t)(sy1 - sy0) } };
    vkCmdSetScissor(cb, 0, 1, &sc);
    float bc[4] = { s->fix / 128.0f, s->fix / 128.0f, s->fix / 128.0f, s->fix / 128.0f };
    vkCmdSetBlendConstants(cb, bc);
    PushC pc; memset(&pc, 0, sizeof pc);
    pc.rt[0] = 2.0f / (float)r->col.w; pc.rt[1] = 2.0f / (float)r->col.h; pc.rt[2] = (float)N; pc.rt[3] = (0.5f - 0.5f * (float)N) * uv_shift_k();
    pc.flags = s->flags | ((uint32_t)passmode << HWF_PASS_SHIFT); pc.aref = s->aref; pc.fogcol = s->fogcol; pc.blend = s->blend;
    memcpy(pc.region, s->region, sizeof pc.region);
    pc.tex[0] = s->texw; pc.tex[1] = s->texh; pc.tex[2] = s->tex_kind == 2 ? (float)rts[s->tex].n : 1.0f;
    pc.zp[0] = s->zscale; pc.zp[1] = s->zmax; pc.zp[2] = s->ta0;
    if (r->mv) { pc.zp[3] = mv_zfar; pc.tex[3] = mv_zinv; }
    pc.fbmsk = s->fbmsk;
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, (uint32_t)count, 1, (uint32_t)(first / sizeof(HwVertex)), 0);
    hw_stat_draws++;
    r->ver++;
}
static void emit_batch(void) {
    if (!bcount) return;
    HwState s = bstate; size_t first = bfirst; int count = bcount;
    bcount = 0;
    begin_cb();
    Rt *r = &rts[s.rt];
    Img *tex = 0;
    int self = s.tex_kind == 2 && s.tex == s.rt;
    if (s.tex_kind == 1) tex = &texs[s.tex];
    else if (s.tex_kind == 2 && !self) {
        if (rendering) end_rendering();
        tex = &rts[s.tex].col;
        to_layout(tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (s.afail >= 4) { draw_range(&s, first, count, s.afail - 2, s.cmask, s.zwrite, tex, 0); return; }     /* motion-vector pass: reactive value of failing (4) / passing (5) fragments */
    if (!self && !s.need_dst && !s.afail) { draw_range(&s, first, count, 0, s.cmask, s.zwrite, tex, 0); return; }
    if (!self && !s.need_dst && !exact_afail) {
        /* Alpha-fail modes, whole batch at a time: first the fragments that
         * pass (normal writes), then the ones that fail (colour and/or depth
         * as the mode says). Exact per pixel except where two primitives of
         * the batch overlap and a failing fragment of the earlier one meets a
         * fragment of the later one: their order is exchanged there.
         * ROR_HW_EXACT_AFAIL=1 draws primitive by primitive instead. */
        int cm = s.afail == 2 ? 0 : s.afail == 3 ? (s.cmask & 7) : s.cmask, zw = s.afail == 2 ? s.zwrite : 0;
        draw_range(&s, first, count, 1, s.cmask, s.zwrite, tex, 0);
        if (cm || zw) draw_range(&s, first, count, 2, cm, zw, tex, 0);
        return;
    }
    /* One primitive at a time: the fragment shader needs the pixels as they
     * were before this primitive (it samples its own target and/or blends in
     * the shader), or the alpha-fail pass of one primitive must come before
     * the next primitive. Pixels of one primitive never overlap each other. */
    const HwVertex *v = (const HwVertex *)(vmap + first);
    for (int i = 0; i + 3 <= count; i += 3) {
        Img *dst = 0;
        if (self) {                              /* snapshot of the whole target (the texture can be read anywhere) */
            if (r->copy_ver != r->ver) { copy_rt(r, 0, 0, r->col.w, r->col.h); r->copy_ver = r->ver; }
            tex = &r->copy; dst = &r->copy;
        } else if (s.need_dst) {                 /* only the pixels the primitive can touch */
            float x0 = v[i].x, x1 = v[i].x, y0 = v[i].y, y1 = v[i].y;
            for (int k = 1; k < 3; k++) { if (v[i + k].x < x0) x0 = v[i + k].x; if (v[i + k].x > x1) x1 = v[i + k].x; if (v[i + k].y < y0) y0 = v[i + k].y; if (v[i + k].y > y1) y1 = v[i + k].y; }
            int ax = (int)x0 - 1, ay = (int)y0 - 1, bx = (int)x1 + 2, by = (int)y1 + 2;
            if (ax < s.sx0) ax = s.sx0; if (ay < s.sy0) ay = s.sy0; if (bx > s.sx1 + 1) bx = s.sx1 + 1; if (by > s.sy1 + 1) by = s.sy1 + 1;
            copy_rt(r, ax * r->n, ay * r->n, bx * r->n, by * r->n);
            r->copy_ver = 0;
            dst = &r->copy;
        }
        draw_range(&s, first + (size_t)i * sizeof(HwVertex), 3, s.afail ? 1 : 0, s.cmask, s.zwrite, tex, dst);
        if (s.afail) {
            int cm = s.afail == 2 ? 0 : s.afail == 3 ? (s.cmask & 7) : s.cmask, zw = s.afail == 2 ? s.zwrite : 0;
            if (self && r->copy_ver != r->ver) { copy_rt(r, 0, 0, r->col.w, r->col.h); r->copy_ver = r->ver; }
            if (cm || zw) draw_range(&s, first + (size_t)i * sizeof(HwVertex), 3, 2, cm, zw, tex, dst);
        }
    }
}
void hw_draw(const HwState *s, const HwVertex *v, int n) {
    size_t bytes = (size_t)n * sizeof(HwVertex);
    if (bcount && memcmp(s, &bstate, sizeof *s) != 0) emit_batch();
    if (voff + bytes > VB_BYTES) hw_flush();
    if (!bcount) { bstate = *s; bfirst = voff; }
    memcpy(vmap + voff, v, bytes);
    voff += bytes; bcount += n;
}
