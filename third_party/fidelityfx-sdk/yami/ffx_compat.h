// Force-included into every yami_fsr3 source. Keeps upstream FidelityFX files byte-identical.
#pragma once

#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>

#if !defined(_WIN32)
// MSVC CRT functions used by the upstream SDK, supplied only for non-Windows builds.
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <codecvt>
#include <locale>
#include <string>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

inline int wcscpy_s(wchar_t* destination, std::size_t size, const wchar_t* source)
{
    if (!destination || !size) return EINVAL;
    const std::size_t length = source ? std::wcslen(source) : 0;
    if (!source || length >= size) { destination[0] = L'\0'; return source ? ERANGE : EINVAL; }
    std::wmemcpy(destination, source, length + 1);
    return 0;
}
template <std::size_t Size> int wcscpy_s(wchar_t (&destination)[Size], const wchar_t* source) { return wcscpy_s(destination, Size, source); }

inline int strcpy_s(char* destination, std::size_t size, const char* source)
{
    if (!destination || !size) return EINVAL;
    const std::size_t length = source ? std::strlen(source) : 0;
    if (!source || length >= size) { destination[0] = '\0'; return source ? ERANGE : EINVAL; }
    std::memcpy(destination, source, length + 1);
    return 0;
}

// Converts at most count bytes; *converted includes the terminator, as on MSVC.
inline int wcstombs_s(std::size_t* converted, char* destination, std::size_t size, const wchar_t* source, std::size_t count)
{
    if (converted) *converted = 0;
    if (!destination || !size || !source) return EINVAL;
    destination[0] = '\0';
    const std::size_t needed = std::wcstombs(nullptr, source, 0);
    if (needed == static_cast<std::size_t>(-1)) return EILSEQ;
    const std::size_t limit = needed < count ? needed : count;
    if (limit >= size) return ERANGE;
    const std::size_t written = std::wcstombs(destination, source, limit);
    if (written == static_cast<std::size_t>(-1)) { destination[0] = '\0'; return EILSEQ; }
    destination[written] = '\0';
    if (converted) *converted = written + 1;
    return 0;
}

inline int sprintf_s(char* buffer, std::size_t size, const char* format, ...)
{
    if (!buffer || !size) return -1;
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    if (length < 0 || static_cast<std::size_t>(length) >= size) { buffer[0] = '\0'; return -1; }
    return length;
}
#endif

// ffx_message.cpp's non-Windows branch uses FFX_UNUSED without including ffx_util.h.
#include <FidelityFX/host/ffx_util.h>

// The game loads Vulkan through SDL. ffx_vk.cpp is compiled with VK_NO_PROTOTYPES and its direct
// vk* calls are redirected to yami-owned pointers filled by yami_ffx_load_vulkan().
#include <vulkan/vulkan.h>
#define YAMI_FFX_VK_FUNCTIONS(X) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetDeviceProcAddr) X(vkCreateBuffer)
#define YAMI_FFX_VK_DECLARE(name) extern PFN_##name yami_ffx_##name;
YAMI_FFX_VK_FUNCTIONS(YAMI_FFX_VK_DECLARE)
#undef YAMI_FFX_VK_DECLARE
#define vkGetPhysicalDeviceFeatures yami_ffx_vkGetPhysicalDeviceFeatures
#define vkGetPhysicalDeviceFeatures2 yami_ffx_vkGetPhysicalDeviceFeatures2
#define vkGetPhysicalDeviceProperties yami_ffx_vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceProperties2 yami_ffx_vkGetPhysicalDeviceProperties2
#define vkGetPhysicalDeviceMemoryProperties yami_ffx_vkGetPhysicalDeviceMemoryProperties
#define vkEnumerateDeviceExtensionProperties yami_ffx_vkEnumerateDeviceExtensionProperties
#define vkGetDeviceProcAddr yami_ffx_vkGetDeviceProcAddr
#define vkCreateBuffer yami_ffx_vkCreateBuffer
