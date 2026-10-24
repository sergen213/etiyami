#pragma once
#include <vulkan/vulkan.h>
#include <FidelityFX/host/ffx_interface.h>

// Resolves the Vulkan entry points the FidelityFX VK backend calls directly. Call once per instance
// before ffxGetInterfaceVK. Returns false (FSR unavailable) when any entry point is missing.
bool yami_ffx_load_vulkan(PFN_vkGetInstanceProcAddr get_instance_proc, VkInstance instance);

// Makes the backend report wave32, no FP16, no synchronization2, no buffer markers and no device-coherent
// memory, so every effect requests only the generated FP32/wave32 permutations and the features the game's
// device enables. Apply to every FfxInterface returned by ffxGetInterfaceVK before creating a context.
void yami_ffx_clamp_capabilities(FfxInterface& backend);

// VkDeviceContext::vkDeviceProcAddr for the backend (after yami_ffx_load_vulkan): resolves through the
// device, mapping the VK_KHR_get_memory_requirements2 alias the backend asks for to the Vulkan 1.1 core
// entry point so the extension need not be enabled.
PFN_vkVoidFunction VKAPI_PTR yami_ffx_device_proc(VkDevice device, const char* name);
