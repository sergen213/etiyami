// Shadows the upstream header (yami/ precedes include/ on the include path) so the library and the
// engine agree on FfxFsr3UpscalerContext. The SDK sizes the opaque context for 2-byte wchar_t; the
// private context embeds many wchar_t name arrays, so 4-byte wchar_t (Linux) needs proportionally more.
#pragma once
#include "../../../include/FidelityFX/host/ffx_types.h"
#undef FFX_SDK_DEFAULT_CONTEXT_SIZE
#define FFX_SDK_DEFAULT_CONTEXT_SIZE (1024 * 128 * sizeof(wchar_t) / 2)
