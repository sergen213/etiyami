#include "ffx_compat.h"
#include "yami_ffx_vulkan.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>

#define YAMI_FFX_VK_DEFINE(name) PFN_##name yami_ffx_##name = nullptr;
YAMI_FFX_VK_FUNCTIONS(YAMI_FFX_VK_DEFINE)

bool yami_ffx_load_vulkan(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance)
{
    if (!get_instance_proc || !instance) return false;
    bool loaded = true;
#define YAMI_FFX_VK_LOAD(name) loaded = (yami_ffx_##name = reinterpret_cast<PFN_##name>(get_instance_proc(instance, #name))) && loaded;
    YAMI_FFX_VK_FUNCTIONS(YAMI_FFX_VK_LOAD)
    return loaded;
}

namespace {
// Every ffxGetInterfaceVK interface holds the same backend query, so one saved pointer serves them all.
FfxGetDeviceCapabilitiesFunc backend_capabilities = nullptr;

// The backend reports what the device *supports*; the generated blobs and the game's device cover only this.
FfxErrorCode enabled_capabilities(FfxInterface* backend, FfxDeviceCapabilities* capabilities)
{
    const FfxErrorCode result = backend_capabilities(backend, capabilities);
    capabilities->waveLaneCountMin = capabilities->waveLaneCountMax = 32;
    capabilities->fp16Supported = false;
    capabilities->extendedSynchronizationSupported = false;
    capabilities->bufferMarkerSupported = false;
    capabilities->deviceCoherentMemorySupported = false;
    return result;
}
}

void yami_ffx_clamp_capabilities(FfxInterface& backend)
{
    if (backend.fpGetDeviceCapabilities == enabled_capabilities) return;
    backend_capabilities = backend.fpGetDeviceCapabilities;
    backend.fpGetDeviceCapabilities = enabled_capabilities;
}

PFN_vkVoidFunction VKAPI_PTR yami_ffx_device_proc(VkDevice device, const char* name)
{
    return yami_ffx_vkGetDeviceProcAddr(device, std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") ? name : "vkGetBufferMemoryRequirements2");
}

// ffxGetInterfaceVK stores this entry point; its definition lives in the Win32-only frame-interpolation
// swapchain, which is not vendored (the engine dispatches frame interpolation and presents itself).
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*)
{
    return FFX_ERROR_INVALID_ARGUMENT;
}
