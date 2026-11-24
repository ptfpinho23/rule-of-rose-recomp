/* sl_min.h — the part of NVIDIA's Streamline API that gs_hw.c uses to run
 * DLSS Super Resolution through sl.interposer.dll / sl.common.dll /
 * sl.dlss.dll (which load nvngx_dlss.dll). Declarations transcribed to C from
 * the Streamline SDK 2.14.16 headers (github.com/NVIDIA-RTX/Streamline, tag
 * v2.14.16): include/sl_struct.h, sl_core_types.h, sl_core_api.h,
 * sl_consts.h, sl_result.h, sl_version.h, sl_dlss.h, sl_dlss_g.h.
 * The interface is a set of C-linkage functions exported by sl.interposer.dll
 * that take pointers to plain structures, each starting with a common header
 * (next pointer, a GUID naming the structure, a version); C++ references are
 * pointers, `bool` is one byte, enumerations are 32 bits unless noted. Only
 * what is used here is declared. The SDK headers are not needed to build.
 *
 * Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */
#ifndef ROR_SL_MIN_H
#define ROR_SL_MIN_H
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

/* ---- sl_struct.h ---- */
typedef struct SlGuid { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } SlGuid;
typedef struct SlBase { struct SlBase *next; SlGuid type; size_t version; } SlBase;
#define SL_HEAD(g, v) { 0, g, v }

/* ---- sl_version.h ---- */
#define SL_SDK_VERSION ((2ull << 48) | (14ull << 32) | (16ull << 16) | 0xfedcull)
typedef struct SlVersion { uint32_t major, minor, build; } SlVersion;

/* ---- sl_result.h (enum class Result) ---- */
enum { SL_OK = 0, SL_ERR_DRIVER_OUT_OF_DATE = 2, SL_ERR_OS_OUT_OF_DATE = 3, SL_ERR_OS_DISABLED_HWS = 4, SL_ERR_NO_SUPPORTED_ADAPTER = 6,
       SL_ERR_ADAPTER_NOT_SUPPORTED = 7, SL_ERR_NO_PLUGINS = 8, SL_ERR_FEATURE_MISSING = 31, SL_ERR_FEATURE_NOT_SUPPORTED = 32 };
typedef int SlResult;

/* ---- sl_core_types.h ---- */
enum { SL_FEATURE_DLSS = 0, SL_FEATURE_REFLEX = 3, SL_FEATURE_PCL = 4, SL_FEATURE_DLSS_G = 1000 };
enum { SL_BUFFER_DEPTH = 0, SL_BUFFER_MVEC = 1, SL_BUFFER_SCALING_INPUT = 3, SL_BUFFER_SCALING_OUTPUT = 4 };
enum { SL_LOG_OFF = 0, SL_LOG_DEFAULT = 1, SL_LOG_VERBOSE = 2 };
enum { SL_LOGTYPE_INFO = 0, SL_LOGTYPE_WARN = 1, SL_LOGTYPE_ERROR = 2 };
enum { SL_PREF_DISABLE_CL_STATE_TRACKING = 1 << 0, SL_PREF_DISABLE_DEBUG_TEXT = 1 << 1, SL_PREF_USE_MANUAL_HOOKING = 1 << 2, SL_PREF_ALLOW_OTA = 1 << 3,
       SL_PREF_BYPASS_OS_VERSION_CHECK = 1 << 4, SL_PREF_LOAD_DOWNLOADED_PLUGINS = 1 << 6, SL_PREF_FRAME_BASED_TAGGING = 1 << 7 };
enum { SL_ENGINE_CUSTOM = 0 };
enum { SL_RENDER_API_VULKAN = 2 };
enum { SL_LIFECYCLE_ONLY_VALID_NOW = 0, SL_LIFECYCLE_VALID_UNTIL_PRESENT = 1, SL_LIFECYCLE_VALID_UNTIL_EVALUATE = 2 };
typedef struct SlExtent { uint32_t top, left, width, height; } SlExtent;
#define SL_GUID_RESOURCE { 0x3a9d70cf, 0x2418, 0x4b72, { 0x83, 0x91, 0x13, 0xf8, 0x72, 0x1c, 0x72, 0x61 } }
typedef struct SlResource {            /* kStructVersion1; Vulkan: VkImage, VkDeviceMemory, VkImageView, VkImageLayout, VkFormat, flags and usage of the image */
    SlBase h;
    char type;                         /* ResourceType: 0 = eTex2d */
    void *native, *memory, *view;
    uint32_t state, width, height, nativeFormat, mipLevels, arrayLayers;
    uint64_t gpuVirtualAddress;
    uint32_t flags, usage;
    uint16_t internalFlags, reserved;
} SlResource;
#define SL_GUID_RESOURCE_TAG { 0x4c6a5aad, 0xb445, 0x496c, { 0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53 } }
typedef struct SlResourceTag { SlBase h; SlResource *resource; uint32_t type; int lifecycle; SlExtent extent; } SlResourceTag;      /* kStructVersion1 */
typedef void (*SlLogCallback)(uint32_t type, const char *msg);
#define SL_GUID_PREFERENCES { 0x1ca10965, 0xbf8e, 0x432b, { 0x8d, 0xa1, 0x67, 0x16, 0xd8, 0x79, 0xfb, 0x14 } }
typedef struct SlPreferences {         /* kStructVersion1 */
    SlBase h;
    uint8_t showConsole; uint32_t logLevel;
    const wchar_t **pathsToPlugins; uint32_t numPathsToPlugins;
    const wchar_t *pathToLogsAndData;
    void *allocateCallback, *releaseCallback; SlLogCallback logMessageCallback;
    uint64_t flags;
    const uint32_t *featuresToLoad; uint32_t numFeaturesToLoad;
    uint32_t applicationId, engine;
    const char *engineVersion, *projectId;
    uint32_t renderAPI;
} SlPreferences;
#define SL_GUID_VIEWPORT { 0x171b6435, 0x9b3c, 0x4fc8, { 0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4 } }
typedef struct SlViewport { SlBase h; uint32_t value; } SlViewport;                   /* kStructVersion1 */
#define SL_GUID_FEATURE_VERSION { 0x6d5b51f0, 0x76b, 0x486d, { 0x99, 0x95, 0x5a, 0x56, 0x10, 0x43, 0xf5, 0xc1 } }
typedef struct SlFeatureVersion { SlBase h; SlVersion versionSL, versionNGX; } SlFeatureVersion;      /* kStructVersion1 */
#define SL_GUID_ADAPTER_INFO { 0x677315f, 0xa746, 0x4492, { 0x9f, 0x42, 0xcb, 0x61, 0x42, 0xc9, 0xc3, 0xd4 } }
typedef struct SlAdapterInfo { SlBase h; uint8_t *deviceLUID; uint32_t deviceLUIDSizeInBytes; void *vkPhysicalDevice; } SlAdapterInfo;   /* kStructVersion1 */
typedef struct SlFrameToken SlFrameToken;             /* opaque */

/* ---- sl_consts.h ---- */
#define SL_INVALID_FLOAT 3.40282346638528859811704183484516925440e38f
enum { SL_FALSE = 0, SL_TRUE = 1, SL_INVALID = 2 };  /* Boolean : char */
#define SL_GUID_CONSTANTS { 0xdcd35ad7, 0x4e4a, 0x4bad, { 0xa9, 0xc, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe } }
typedef struct SlConstants {           /* kStructVersion2 */
    SlBase h;
    float cameraViewToClip[16], clipToCameraView[16], clipToLensClip[16], clipToPrevClip[16], prevClipToClip[16];     /* row-major, without jitter */
    float jitterOffset[2], mvecScale[2], cameraPinholeOffset[2];
    float cameraPos[3], cameraUp[3], cameraRight[3], cameraFwd[3];
    float cameraNear, cameraFar, cameraFOV, cameraAspectRatio, motionVectorsInvalidValue;
    char depthInverted, cameraMotionIncluded, motionVectors3D, reset, orthographicProjection, motionVectorsDilated, motionVectorsJittered;
    float minRelativeLinearDepthObjectSeparation;
} SlConstants;

/* ---- sl_dlss.h ---- */
enum { SL_DLSS_OFF = 0, SL_DLSS_MAX_PERFORMANCE = 1, SL_DLSS_BALANCED = 2, SL_DLSS_MAX_QUALITY = 3, SL_DLSS_ULTRA_PERFORMANCE = 4, SL_DLSS_ULTRA_QUALITY = 5, SL_DLSS_DLAA = 6 };
enum { SL_DLSS_PRESET_DEFAULT = 0, SL_DLSS_PRESET_J = 10, SL_DLSS_PRESET_K = 11, SL_DLSS_PRESET_L = 12, SL_DLSS_PRESET_M = 13 };
#define SL_GUID_DLSS_OPTIONS { 0x6ac826e4, 0x4c61, 0x4101, { 0xa9, 0x2d, 0x63, 0x8d, 0x42, 0x10, 0x57, 0xb8 } }
typedef struct SlDLSSOptions {         /* kStructVersion3 */
    SlBase h;
    uint32_t mode, outputWidth, outputHeight;
    float sharpness /* deprecated: 0 */, preExposure, exposureScale;
    char colorBuffersHDR, indicatorInvertAxisX, indicatorInvertAxisY;
    uint32_t dlaaPreset, qualityPreset, balancedPreset, performancePreset, ultraPerformancePreset, ultraQualityPreset;
    char useAutoExposure, alphaUpscalingEnabled;
} SlDLSSOptions;
#define SL_GUID_DLSS_SETTINGS { 0xef1d0957, 0xfd58, 0x4df7, { 0xb5, 0x4, 0x8b, 0x69, 0xd8, 0xaa, 0x6b, 0x76 } }
typedef struct SlDLSSOptimalSettings { SlBase h; uint32_t optimalRenderWidth, optimalRenderHeight; float optimalSharpness;
                                       uint32_t renderWidthMin, renderHeightMin, renderWidthMax, renderHeightMax; } SlDLSSOptimalSettings;   /* kStructVersion1 */
#define SL_GUID_DLSS_STATE { 0x9366b056, 0x8c01, 0x463c, { 0xbb, 0x91, 0xe6, 0x87, 0x82, 0x63, 0x6c, 0xe9 } }
typedef struct SlDLSSState { SlBase h; uint64_t estimatedVRAMUsageInBytes; } SlDLSSState;                                              /* kStructVersion1 */

/* ---- sl_core_api.h: exports of sl.interposer.dll ---- */
typedef SlResult (*PfnSlInit)(const SlPreferences *pref, uint64_t sdkVersion);
typedef SlResult (*PfnSlShutdown)(void);
typedef SlResult (*PfnSlIsFeatureSupported)(uint32_t feature, const SlAdapterInfo *adapter);
typedef SlResult (*PfnSlSetFeatureLoaded)(uint32_t feature, uint8_t loaded);
typedef SlResult (*PfnSlGetFeatureVersion)(uint32_t feature, SlFeatureVersion *version);
typedef SlResult (*PfnSlGetFeatureFunction)(uint32_t feature, const char *name, void **function);
typedef SlResult (*PfnSlGetNewFrameToken)(SlFrameToken **token, const uint32_t *frameIndex);
typedef SlResult (*PfnSlSetConstants)(const SlConstants *values, const SlFrameToken *frame, const SlViewport *viewport);
typedef SlResult (*PfnSlEvaluateFeature)(uint32_t feature, const SlFrameToken *frame, const SlBase **inputs, uint32_t numInputs, void *cmdBuffer);
typedef SlResult (*PfnSlFreeResources)(uint32_t feature, const SlViewport *viewport);
/* feature functions, obtained by name with slGetFeatureFunction(SL_FEATURE_DLSS, ...) */
typedef SlResult (*PfnSlDLSSGetOptimalSettings)(const SlDLSSOptions *options, SlDLSSOptimalSettings *settings);
typedef SlResult (*PfnSlDLSSGetState)(const SlViewport *viewport, SlDLSSState *state);
typedef SlResult (*PfnSlDLSSSetOptions)(const SlViewport *viewport, const SlDLSSOptions *options);
#endif
