/* ffx_min.h — the part of the AMD FidelityFX API (ffx-api) that gs_hw.c uses
 * to drive the temporal upscaler in amd_fidelityfx_loader_vk.dll /
 * amd_fidelityfx_upscaler_vk.dll (FSR 3.1; FSR 4 where the DLL or the driver
 * provides it). Declarations transcribed from FidelityFX SDK 2.4.0
 * (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag v2.4.0):
 *   Kits/FidelityFX/api/include/ffx_api.h, ffx_api_types.h, vk/ffx_api_vk.h
 *   Kits/FidelityFX/upscalers/include/ffx_upscale.h
 * The interface is a C ABI of five functions taking chains of tagged
 * structures; only the structures used here are declared. The SDK headers
 * themselves are not needed to build.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE. */
#ifndef ROR_FFX_MIN_H
#define ROR_FFX_MIN_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- ffx_api.h ---- */
enum { FFX_API_RETURN_OK = 0 };
typedef void *ffxContext;
typedef uint32_t ffxReturnCode_t;
typedef uint64_t ffxStructType_t;
typedef struct ffxApiHeader { ffxStructType_t type; struct ffxApiHeader *pNext; } ffxApiHeader;
typedef void (*ffxApiMessage)(uint32_t type, const wchar_t *message);
typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext *context, ffxApiHeader *desc, const void *memCb);
typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext *context, const void *memCb);
typedef ffxReturnCode_t (*PfnFfxConfigure)(ffxContext *context, const ffxApiHeader *desc);
typedef ffxReturnCode_t (*PfnFfxQuery)(ffxContext *context, ffxApiHeader *desc);
typedef ffxReturnCode_t (*PfnFfxDispatch)(ffxContext *context, const ffxApiHeader *desc);

#define FFX_API_QUERY_DESC_TYPE_GET_VERSIONS 4u
struct ffxQueryDescGetVersions { ffxApiHeader header; uint64_t createDescType; void *device; uint64_t *outputCount; uint64_t *versionIds; const char **versionNames; };
#define FFX_API_DESC_TYPE_OVERRIDE_VERSION 5u
struct ffxOverrideVersion { ffxApiHeader header; uint64_t versionId; };
#define FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION 6u
struct ffxQueryGetProviderVersion { ffxApiHeader header; uint64_t versionId; const char *versionName; };

#define FFX_API_EFFECT_ID_UPSCALE 0x00010000u

/* ---- ffx_api_types.h ---- */
enum FfxApiSurfaceFormat {
    FFX_API_SURFACE_FORMAT_UNKNOWN, FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS, FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT,
    FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT,
    FFX_API_SURFACE_FORMAT_R32G32_FLOAT, FFX_API_SURFACE_FORMAT_R8_UINT, FFX_API_SURFACE_FORMAT_R32_UINT,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB, FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS, FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM,
    FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB, FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT, FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM,
    FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_SURFACE_FORMAT_R16G16_UINT, FFX_API_SURFACE_FORMAT_R16G16_SINT,
    FFX_API_SURFACE_FORMAT_R16_FLOAT, FFX_API_SURFACE_FORMAT_R16_UINT, FFX_API_SURFACE_FORMAT_R16_UNORM,
    FFX_API_SURFACE_FORMAT_R16_SNORM, FFX_API_SURFACE_FORMAT_R8_UNORM, FFX_API_SURFACE_FORMAT_R8G8_UNORM,
    FFX_API_SURFACE_FORMAT_R8G8_UINT, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP
};
enum { FFX_API_RESOURCE_USAGE_READ_ONLY = 0, FFX_API_RESOURCE_USAGE_RENDERTARGET = 1 << 0, FFX_API_RESOURCE_USAGE_UAV = 1 << 1 };
enum { FFX_API_RESOURCE_STATE_COMPUTE_READ = 1 << 2, FFX_API_RESOURCE_STATE_PIXEL_READ = 1 << 3,
       FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ = (1 << 3) | (1 << 2) };
enum { FFX_API_RESOURCE_FLAGS_NONE = 0 };
enum { FFX_API_RESOURCE_TYPE_BUFFER, FFX_API_RESOURCE_TYPE_TEXTURE1D, FFX_API_RESOURCE_TYPE_TEXTURE2D };
struct FfxApiDimensions2D { uint32_t width, height; };
struct FfxApiFloatCoords2D { float x, y; };
struct FfxApiResourceDescription {
    uint32_t type, format;
    union { uint32_t width, size; };
    union { uint32_t height, stride; };
    union { uint32_t depth, alignment; };
    uint32_t mipCount, flags, usage;
};
struct FfxApiResource { void *resource; struct FfxApiResourceDescription description; uint32_t state; };

/* ---- vk/ffx_api_vk.h (VkDevice etc. come from the Vulkan headers included before this file) ---- */
typedef struct VkDeviceContext { VkDevice vkDevice; VkPhysicalDevice vkPhysicalDevice; PFN_vkGetDeviceProcAddr vkDeviceProcAddr; } VkDeviceContext;
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK (0x01000000u | 0x03u)
struct ffxCreateBackendVKDesc { ffxApiHeader header; VkDeviceContext deviceContext; };

/* ---- ffx_upscale.h ---- */
#define FFX_UPSCALER_VERSION ((4u << 22) | (1u << 12) | 2u)
enum {
    FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE = 1 << 0, FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = 1 << 1,
    FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = 1 << 2, FFX_UPSCALE_ENABLE_DEPTH_INVERTED = 1 << 3,
    FFX_UPSCALE_ENABLE_DEPTH_INFINITE = 1 << 4, FFX_UPSCALE_ENABLE_AUTO_EXPOSURE = 1 << 5, FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION = 1 << 6,
    FFX_UPSCALE_ENABLE_DEBUG_CHECKING = 1 << 7, FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE = 1 << 8, FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION = 1 << 9
};
enum { FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW = 1 << 0, FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB = 1 << 1, FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ = 1 << 2 };
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE (FFX_API_EFFECT_ID_UPSCALE | 0x00u)
struct ffxCreateContextDescUpscale {
    ffxApiHeader header; uint32_t flags; struct FfxApiDimensions2D maxRenderSize, maxUpscaleSize; ffxApiMessage fpMessage;
};
#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE (FFX_API_EFFECT_ID_UPSCALE | 0x01u)
struct ffxDispatchDescUpscale {
    ffxApiHeader header;
    void *commandList;
    struct FfxApiResource color, depth, motionVectors, exposure, reactive, transparencyAndComposition, output;
    struct FfxApiFloatCoords2D jitterOffset, motionVectorScale;
    struct FfxApiDimensions2D renderSize, upscaleSize;
    bool enableSharpening; float sharpness; float frameTimeDelta; float preExposure;
    bool reset; float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
    uint32_t flags;
};
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT (FFX_API_EFFECT_ID_UPSCALE | 0x04u)
struct ffxQueryDescUpscaleGetJitterPhaseCount { ffxApiHeader header; uint32_t renderWidth, displayWidth; int32_t *pOutPhaseCount; };
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET (FFX_API_EFFECT_ID_UPSCALE | 0x05u)
struct ffxQueryDescUpscaleGetJitterOffset { ffxApiHeader header; int32_t index, phaseCount; float *pOutX, *pOutY; };
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION (FFX_API_EFFECT_ID_UPSCALE | 0x0bu)
struct ffxCreateContextDescUpscaleVersion { ffxApiHeader header; uint32_t version; };

/* ---- frame generation (amd_fidelityfx_framegeneration_vk.dll; FSR 3.1.6 frame interpolation) ----
 * framegeneration/include/ffx_framegeneration.h, api/include/ffx_api_types.h. Used without AMD's
 * swapchain wrapper: the context is configured with NO_SWAPCHAIN_CONTEXT_NOTIFY and dispatched by
 * hand, which writes the in-between picture into an image of the caller. */
#define FFX_API_EFFECT_ID_FRAMEGENERATION 0x00020000u
#define FFX_FRAMEGENERATION_VERSION ((4u << 22) | (0u << 12) | 2u)
enum { FFX_API_RESOURCE_STATE_UNORDERED_ACCESS = 1 << 1 };
enum { FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB = 0 };
struct FfxApiRect2D { int32_t left, top, width, height; };
struct FfxApiEffectMemoryUsage { uint64_t totalUsageInBytes, aliasableUsageInBytes; };
enum { FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED = 1 << 3, FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING = 1 << 6 };
enum { FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES = 1 << 0, FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW = 1 << 2, FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY = 1 << 3 };
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION (FFX_API_EFFECT_ID_FRAMEGENERATION | 0x01u)
struct ffxCreateContextDescFrameGeneration { ffxApiHeader header; uint32_t flags; struct FfxApiDimensions2D displaySize, maxRenderSize; uint32_t backBufferFormat; };
#define FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION (FFX_API_EFFECT_ID_FRAMEGENERATION | 0x02u)
struct ffxConfigureDescFrameGeneration {
    ffxApiHeader header; void *swapChain;
    void *presentCallback, *presentCallbackUserContext, *frameGenerationCallback, *frameGenerationCallbackUserContext;
    bool frameGenerationEnabled, allowAsyncWorkloads;
    struct FfxApiResource HUDLessColor; uint32_t flags; bool onlyPresentGenerated; struct FfxApiRect2D generationRect; uint64_t frameID;
};
#define FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION (FFX_API_EFFECT_ID_FRAMEGENERATION | 0x03u)
struct ffxDispatchDescFrameGeneration {
    ffxApiHeader header; void *commandList;
    struct FfxApiResource presentColor, outputs[4];
    uint32_t numGeneratedFrames; bool reset; uint32_t backbufferTransferFunction; float minMaxLuminance[2];
    struct FfxApiRect2D generationRect; uint64_t frameID;
};
#define FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE (FFX_API_EFFECT_ID_FRAMEGENERATION | 0x07u)
struct ffxQueryDescFrameGenerationGetGPUMemoryUsage { ffxApiHeader header; struct FfxApiEffectMemoryUsage *gpuMemoryUsageFrameGeneration; };
#define FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 (FFX_API_EFFECT_ID_FRAMEGENERATION | 0x0cu)
struct ffxDispatchDescFrameGenerationPrepareV2 {
    ffxApiHeader header; uint64_t frameID; uint32_t flags; void *commandList;
    struct FfxApiDimensions2D renderSize; struct FfxApiFloatCoords2D jitterOffset, motionVectorScale;
    float frameTimeDelta; bool reset; float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor;
    struct FfxApiResource depth, motionVectors;
    float cameraPosition[3], cameraUp[3], cameraRight[3], cameraForward[3];
};
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION (FFX_API_EFFECT_ID_FRAMEGENERATION | 0x0eu)
struct ffxCreateContextDescFrameGenerationVersion { ffxApiHeader header; uint32_t version; };
/* ffx_upscale.h: memory used by an upscaler context */
#define FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE (FFX_API_EFFECT_ID_UPSCALE | 0x08u)
struct ffxQueryDescUpscaleGetGPUMemoryUsage { ffxApiHeader header; struct FfxApiEffectMemoryUsage *gpuMemoryUsageUpscaler; };
#endif
