// Replaces game shaders with AWTweaks' own, in families:
//  - shadow: ShadowBuffer.rfx "update_shadow_buffer_PS" (the screen-space sun/moon shadow
//    mask), from src/shaders/shadow_buffer.hlsl: rebuilt bit-identical, plus optional soft
//    and contact shadows the main DLL switches on (see the .hlsl);
//  - AO: HBAO.rfx "coarseAOPS" (NVIDIA HBAO+), from src/shaders/hbao_coarse.hlsl;
//  - SSS: SSSSS.rfx "sss_blurPS" (skin subsurface scattering), so far only a diagnostic
//    view from src/shaders/sss_debug.hlsl.
// ReShade reports every pipeline the game creates; when its pixel shader is one of the
// originals (matched by the DXBC container digest and size) we swap in ours.
// Each family's mode comes from awtweaks.ini [Shaders], read at game start.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <reshade.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "../shaders/hbao_blobs.h"
#include "../shaders/shadow_buffer_blobs.h"
#include "../shaders/sss_blobs.h"
#include "shader_replace.h"

namespace shader_replace {
namespace {

using namespace reshade::api;

struct Variant {
    const char* name;
    uint8_t digest[16];  // DXBC container hash of the game's shader (bytes 4..20)
    size_t size;
    const uint8_t* enhanced;  // null: Enhanced runs the original
    size_t enhancedSize;
    const uint8_t* debug;
    size_t debugSize;
};

// Hashes from the shaderbins of game build 34885.
const Variant kShadowVariants[] = {
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

const Variant kAoVariants[] = {
    {"4 directions",
     {0xbd, 0xad, 0x6c, 0xe9, 0xd2, 0x3c, 0xbb, 0xf7, 0x7e, 0xa8, 0xf3, 0x30, 0x46, 0xb8, 0x39, 0x90},
     12087, kAoDir4, sizeof(kAoDir4), kAoDir4Debug, sizeof(kAoDir4Debug)},
    {"6 directions",
     {0xa3, 0xc0, 0x9c, 0x93, 0x61, 0xf3, 0x02, 0x40, 0x8a, 0xe6, 0x48, 0x1b, 0xd9, 0x36, 0x72, 0xda},
     15315, kAoDir6, sizeof(kAoDir6), kAoDir6Debug, sizeof(kAoDir6Debug)},
    {"8 directions",
     {0x24, 0x1c, 0x9f, 0xf7, 0x02, 0xe0, 0x9e, 0x0c, 0x7c, 0x00, 0x9d, 0xe8, 0x8c, 0x8c, 0x90, 0x67},
     18527, kAoDir8, sizeof(kAoDir8), kAoDir8Debug, sizeof(kAoDir8Debug)},
};

const Variant kSssVariants[] = {
    {"blur",
     {0x98, 0x2a, 0x42, 0x4e, 0x05, 0x89, 0x0f, 0x5b, 0xec, 0xdd, 0xaf, 0x64, 0x08, 0xe6, 0x9b, 0xb6},
     10239, nullptr, 0, kSssDebug, sizeof(kSssDebug)},
};

struct FamilyState {
    const char* label;      // for the log
    const wchar_t* iniKey;  // [Shaders] key
    const Variant* variants;
    int count;
    Mode mode = Mode::Original;
    std::atomic<int> replaced{0};
    std::atomic<int> missed{0};
    std::atomic<unsigned> seen{0};  // bit per variant
};

FamilyState g_families[] = {
    {"shadow", L"ShadowShader", kShadowVariants, sizeof(kShadowVariants) / sizeof(Variant)},
    {"AO", L"AOShader", kAoVariants, sizeof(kAoVariants) / sizeof(Variant)},
    {"SSS", L"SSSShader", kSssVariants, sizeof(kSssVariants) / sizeof(Variant)},
};
constexpr int kNumFamilies = sizeof(g_families) / sizeof(g_families[0]);
bool g_registered = false;

void Log(reshade::log::level level, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    reshade::log::message(level, buf);
}

// Which family and variant a shader blob is, or false.
bool Find(const void* code, size_t size, int* family, int* variant) {
    if (!code || size < 20 || memcmp(code, "DXBC", 4) != 0) return false;
    const uint8_t* digest = static_cast<const uint8_t*>(code) + 4;
    for (int f = 0; f < kNumFamilies; ++f)
        for (int i = 0; i < g_families[f].count; ++i) {
            const Variant& v = g_families[f].variants[i];
            if (size == v.size && memcmp(digest, v.digest, 16) == 0) {
                *family = f;
                *variant = i;
                return true;
            }
        }
    return false;
}

Mode ReadMode(const wchar_t* key) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return Mode::Original;
    std::wstring ini(path, n);
    ini.resize(ini.find_last_of(L"\\/") + 1);
    ini += L"awtweaks.ini";
    wchar_t v[64] = {};
    GetPrivateProfileStringW(L"Shaders", key, L"Original", v, 64, ini.c_str());
    // "Rebuilt" was the name before the enhanced options existed; same shader.
    if (_wcsicmp(v, L"Enhanced") == 0 || _wcsicmp(v, L"Rebuilt") == 0) return Mode::Enhanced;
    if (_wcsicmp(v, L"Debug") == 0) return Mode::Debug;
    return Mode::Original;
}

// The shader_desc lives in ReShade's copy of the game's description, so editing it
// changes what the driver compiles. Our blobs are static, so they outlive the call.
bool OnCreatePipeline(device* dev, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects) {
    if (dev->get_api() != device_api::d3d12) return false;
    bool changed = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::pixel_shader || !subobjects[i].data) continue;
        auto* desc = static_cast<shader_desc*>(subobjects[i].data);
        int f, v;
        if (!Find(desc->code, desc->code_size, &f, &v)) continue;
        FamilyState& fam = g_families[f];
        if (fam.mode == Mode::Original) continue;
        const Variant& var = fam.variants[v];
        const bool debug = fam.mode == Mode::Debug;
        if (!debug && !var.enhanced) continue;
        desc->code = debug ? var.debug : var.enhanced;
        desc->code_size = debug ? var.debugSize : var.enhancedSize;
        changed = true;
        int total = ++fam.replaced;
        if (!(fam.seen.fetch_or(1u << v) & (1u << v)))
            Log(reshade::log::level::info, "AWTweaks: replacing %s shader '%s' (%s), pipeline #%d", fam.label,
                var.name, debug ? "debug stripes" : "enhanced", total);
    }
    return changed;
}

// A game shader still present here means a pipeline was created a way we didn't see
// (e.g. loaded from a pipeline library), so it runs the original.
void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline) {
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::pixel_shader || !subobjects[i].data) continue;
        auto* desc = static_cast<const shader_desc*>(subobjects[i].data);
        int f, v;
        if (!Find(desc->code, desc->code_size, &f, &v)) continue;
        FamilyState& fam = g_families[f];
        if (fam.mode != Mode::Original && (fam.mode == Mode::Debug || fam.variants[v].enhanced) && ++fam.missed == 1)
            Log(reshade::log::level::warning, "AWTweaks: %s shader '%s' was created without replacement",
                fam.label, fam.variants[v].name);
    }
}

}  // namespace

void Install() {
    bool any = false;
    for (FamilyState& fam : g_families) {
        fam.mode = ReadMode(fam.iniKey);
        Log(reshade::log::level::info, "AWTweaks: [Shaders] %ls=%s", fam.iniKey,
            ModeName(static_cast<Family>(&fam - g_families)));
        any |= fam.mode != Mode::Original;
    }
    if (!any) return;
    reshade::register_event<reshade::addon_event::create_pipeline>(&OnCreatePipeline);
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    g_registered = true;
}

void Uninstall() {
    if (!g_registered) return;
    reshade::unregister_event<reshade::addon_event::create_pipeline>(&OnCreatePipeline);
    reshade::unregister_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    g_registered = false;
}

Stats GetStats(Family family) {
    const FamilyState& fam = g_families[(int)family];
    Stats s;
    s.mode = fam.mode;
    s.pipelinesReplaced = fam.replaced.load();
    s.pipelinesMissed = fam.missed.load();
    unsigned seen = fam.seen.load();
    s.variantsReplaced = 0;
    for (int i = 0; i < fam.count; ++i) s.variantsReplaced += (seen >> i) & 1;
    s.variantsTotal = fam.count;
    return s;
}

const char* ModeName(Family family) {
    switch (g_families[(int)family].mode) {
        case Mode::Enhanced: return "Enhanced";
        case Mode::Debug: return "Debug";
        default: return "Original";
    }
}

}  // namespace shader_replace
