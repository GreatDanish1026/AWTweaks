// Replaces the game's screen-space sun/moon shadow shaders (ShadowBuffer.rfx,
// "update_shadow_buffer_PS") with our own from src/shaders/shadow_buffer.hlsl: rebuilt bit-identical, plus
// optional soft and contact shadows the main DLL switches on (see the .hlsl).
// ReShade reports every pipeline the game creates; when its pixel shader is one of the
// four originals (matched by the DXBC container digest and size) we swap in ours.
// Mode comes from awtweaks.ini [Shaders] ShadowShader, read at game start.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <reshade.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "../shaders/shadow_buffer_blobs.h"
#include "shader_replace.h"

namespace shader_replace {
namespace {

using namespace reshade::api;

struct Variant {
    const char* name;
    uint8_t digest[16];  // DXBC container hash of the game's shader (bytes 4..20)
    size_t size;
    const uint8_t* enhanced;
    size_t enhancedSize;
    const uint8_t* debug;
    size_t debugSize;
};

// Hashes from ShadowBuffer.shaderbin of game build 34885.
const Variant kVariants[] = {
    {"12-tap fade-out",
     {0x1e, 0xc7, 0xe0, 0xb7, 0xad, 0x2f, 0xb6, 0x83, 0xe0, 0xcc, 0x77, 0x3b, 0x7c, 0x14, 0x3c, 0xd4},
     7091, kSb12End, sizeof(kSb12End), kSb12EndDebug, sizeof(kSb12EndDebug)},
    {"12-tap fade-in",
     {0x08, 0x6a, 0xf4, 0xc2, 0x55, 0x0a, 0x40, 0x38, 0x73, 0xbf, 0x24, 0x2f, 0x36, 0x10, 0x53, 0xb0},
     7051, kSb12Start, sizeof(kSb12Start), kSb12StartDebug, sizeof(kSb12StartDebug)},
    {"10-tap fade-out",
     {0xc5, 0xf8, 0x21, 0x35, 0x1f, 0xe3, 0xad, 0x11, 0x89, 0xb0, 0x4b, 0xd6, 0x54, 0x84, 0x28, 0xaa},
     6791, kSb10End, sizeof(kSb10End), kSb10EndDebug, sizeof(kSb10EndDebug)},
    {"10-tap fade-in",
     {0x08, 0x5a, 0x11, 0x97, 0xb0, 0xb4, 0x4e, 0x7f, 0x9f, 0xb9, 0x8b, 0x58, 0xaf, 0xdb, 0x04, 0xdb},
     6751, kSb10Start, sizeof(kSb10Start), kSb10StartDebug, sizeof(kSb10StartDebug)},
};
constexpr int kNumVariants = sizeof(kVariants) / sizeof(kVariants[0]);

Mode g_mode = Mode::Original;
std::atomic<int> g_replaced{0};
std::atomic<int> g_missed{0};
std::atomic<unsigned> g_variantsSeen{0};  // bit per variant

void Log(reshade::log::level level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    reshade::log::message(level, buf);
}

int Find(const void* code, size_t size) {
    if (!code || size < 20 || memcmp(code, "DXBC", 4) != 0) return -1;
    const uint8_t* digest = static_cast<const uint8_t*>(code) + 4;
    for (int i = 0; i < kNumVariants; ++i)
        if (size == kVariants[i].size && memcmp(digest, kVariants[i].digest, 16) == 0) return i;
    return -1;
}

Mode ReadMode() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return Mode::Original;
    std::wstring ini(path, n);
    ini.resize(ini.find_last_of(L"\\/") + 1);
    ini += L"awtweaks.ini";
    wchar_t v[64] = {};
    GetPrivateProfileStringW(L"Shaders", L"ShadowShader", L"Original", v, 64, ini.c_str());
    // "Rebuilt" was the name before the enhanced options existed; same shader.
    if (_wcsicmp(v, L"Enhanced") == 0 || _wcsicmp(v, L"Rebuilt") == 0) return Mode::Enhanced;
    if (_wcsicmp(v, L"Debug") == 0) return Mode::Debug;
    return Mode::Original;
}

// The shader_desc lives in ReShade's copy of the game's description, so editing it
// changes what the driver compiles. Our blobs are static, so they outlive the call.
bool OnCreatePipeline(device* dev, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects) {
    if (g_mode == Mode::Original || dev->get_api() != device_api::d3d12) return false;
    bool changed = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::pixel_shader || !subobjects[i].data) continue;
        auto* desc = static_cast<shader_desc*>(subobjects[i].data);
        int v = Find(desc->code, desc->code_size);
        if (v < 0) continue;
        const Variant& var = kVariants[v];
        desc->code = g_mode == Mode::Debug ? var.debug : var.enhanced;
        desc->code_size = g_mode == Mode::Debug ? var.debugSize : var.enhancedSize;
        changed = true;
        int total = ++g_replaced;
        if (!(g_variantsSeen.fetch_or(1u << v) & (1u << v)))
            Log(reshade::log::level::info, "AWTweaks: replacing shadow shader '%s' (%s), pipeline #%d", var.name,
                g_mode == Mode::Debug ? "debug stripes" : "enhanced", total);
    }
    return changed;
}

// A game shader still present here means a pipeline was created a way we didn't see
// (e.g. loaded from a pipeline library), so it runs the original.
void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline) {
    if (g_mode == Mode::Original) return;
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::pixel_shader || !subobjects[i].data) continue;
        auto* desc = static_cast<const shader_desc*>(subobjects[i].data);
        int v = Find(desc->code, desc->code_size);
        if (v >= 0 && ++g_missed == 1)
            Log(reshade::log::level::warning, "AWTweaks: shadow shader '%s' was created without replacement",
                kVariants[v].name);
    }
}

}  // namespace

void Install() {
    g_mode = ReadMode();
    Log(reshade::log::level::info, "AWTweaks: [Shaders] ShadowShader=%s", ModeName());
    if (g_mode == Mode::Original) return;
    reshade::register_event<reshade::addon_event::create_pipeline>(&OnCreatePipeline);
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
}

void Uninstall() {
    if (g_mode == Mode::Original) return;
    reshade::unregister_event<reshade::addon_event::create_pipeline>(&OnCreatePipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
}

Stats GetStats() {
    Stats s;
    s.mode = g_mode;
    s.pipelinesReplaced = g_replaced.load();
    s.pipelinesMissed = g_missed.load();
    unsigned seen = g_variantsSeen.load();
    s.variantsReplaced = 0;
    for (int i = 0; i < kNumVariants; ++i) s.variantsReplaced += (seen >> i) & 1;
    s.variantsTotal = kNumVariants;
    return s;
}

const char* ModeName() {
    switch (g_mode) {
        case Mode::Enhanced: return "Enhanced";
        case Mode::Debug: return "Debug";
        default: return "Original";
    }
}

}  // namespace shader_replace
