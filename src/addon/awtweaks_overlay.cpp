// AWTweaks ReShade add-on: an in-game window with live sliders for every engine
// tweakable the main mod (xinput1_4.dll) has found. Open ReShade (Home) and pick
// the "AWTweaks" tab. All state lives in the main DLL; this only draws it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../awtweaks_api.h"
#include "../version.h"
#include "shader_replace.h"

extern "C" __declspec(dllexport) const char* NAME = "AWTweaks";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Live sliders for Alan Wake Remastered engine tweakables (needs AWTweaks' xinput1_4.dll).";

namespace {

struct Api {
    PFN_AWT_GetStatus status = nullptr;
    PFN_AWT_Snapshot snapshot = nullptr;
    PFN_AWT_Set set = nullptr;
    PFN_AWT_Reset reset = nullptr;
    PFN_AWT_SaveToIni save = nullptr;
    PFN_AWT_GetPresets presets = nullptr;
    PFN_AWT_SetPresetActive setPreset = nullptr;
    PFN_AWT_SetModOn setModOn = nullptr;
    PFN_AWT_SaveStartupPresets saveStartup = nullptr;
    PFN_AWT_GetPhoto photo = nullptr;
    PFN_AWT_PhotoAction photoAction = nullptr;
    PFN_AWT_SetTimeScale setTimeScale = nullptr;
    PFN_AWT_StartBenchmark startBench = nullptr;
    PFN_AWT_GetBenchmark getBench = nullptr;
    bool ok() const {
        return status && snapshot && set && reset && save && presets && setPreset && setModOn && saveStartup &&
               photo && photoAction && setTimeScale && startBench && getBench;
    }
} g_api;

// The system xinput1_4.dll is loaded as well (the mod forwards to it), so look
// ours up by its full path next to the game executable.
bool ResolveApi() {
    if (g_api.ok()) return true;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    std::wstring dir(path, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    HMODULE m = GetModuleHandleW((dir + L"xinput1_4.dll").c_str());
    if (!m) return false;
    g_api.status = reinterpret_cast<PFN_AWT_GetStatus>(GetProcAddress(m, "AWT_GetStatus"));
    g_api.snapshot = reinterpret_cast<PFN_AWT_Snapshot>(GetProcAddress(m, "AWT_Snapshot"));
    g_api.set = reinterpret_cast<PFN_AWT_Set>(GetProcAddress(m, "AWT_Set"));
    g_api.reset = reinterpret_cast<PFN_AWT_Reset>(GetProcAddress(m, "AWT_Reset"));
    g_api.save = reinterpret_cast<PFN_AWT_SaveToIni>(GetProcAddress(m, "AWT_SaveToIni"));
    g_api.presets = reinterpret_cast<PFN_AWT_GetPresets>(GetProcAddress(m, "AWT_GetPresets"));
    g_api.setPreset = reinterpret_cast<PFN_AWT_SetPresetActive>(GetProcAddress(m, "AWT_SetPresetActive"));
    g_api.setModOn = reinterpret_cast<PFN_AWT_SetModOn>(GetProcAddress(m, "AWT_SetModOn"));
    g_api.saveStartup = reinterpret_cast<PFN_AWT_SaveStartupPresets>(GetProcAddress(m, "AWT_SaveStartupPresets"));
    g_api.photo = reinterpret_cast<PFN_AWT_GetPhoto>(GetProcAddress(m, "AWT_GetPhoto"));
    g_api.photoAction = reinterpret_cast<PFN_AWT_PhotoAction>(GetProcAddress(m, "AWT_PhotoAction"));
    g_api.setTimeScale = reinterpret_cast<PFN_AWT_SetTimeScale>(GetProcAddress(m, "AWT_SetTimeScale"));
    g_api.startBench = reinterpret_cast<PFN_AWT_StartBenchmark>(GetProcAddress(m, "AWT_StartBenchmark"));
    g_api.getBench = reinterpret_cast<PFN_AWT_GetBenchmark>(GetProcAddress(m, "AWT_GetBenchmark"));
    return g_api.ok();
}

// ReShade is built with MSVC, which returns ImVec2 (it has constructors) through a
// hidden pointer in rcx. This add-on is built with the GNU ABI, which expects an
// 8-byte struct back in rax - calling ImGui::GetContentRegionAvail() through the
// table crashed ReShade. Call that table entry with the MSVC convention instead.
// (Every other table function used here takes structs by reference or returns
// scalars, which both ABIs agree on.)
float AvailWidth() {
    using MsvcRetVec2 = ImVec2*(__cdecl*)(ImVec2*);
    ImVec2 r(0, 0);
    reinterpret_cast<MsvcRetVec2>(
        reinterpret_cast<void*>(imgui_function_table_instance()->GetContentRegionAvail))(&r);
    return r.x;
}

std::vector<AwtInfo> g_items(1500);
char g_filter[128] = "";
bool g_onlyChanged = false;
std::string g_lastSave;

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool Editable(const AwtInfo& t) {
    return t.type == AWT_BOOL || t.type == AWT_FLOAT || t.type == AWT_COLOR || t.type == AWT_LIST;
}

// The n-th option of a list tweakable (items are newline-separated), or "?".
std::string ListOption(const AwtInfo& t, int n) {
    const char* p = t.items;
    for (int i = 0; *p; ++i) {
        const char* e = strchr(p, '\n');
        if (!e) break;
        if (i == n) return std::string(p, e - p);
        p = e + 1;
    }
    return "?";
}

bool Changed(const AwtInfo& t) {
    if (t.source != AWT_SRC_NONE) return true;
    int n = t.type == AWT_COLOR ? 4 : 1;
    return memcmp(t.value, t.original, sizeof(float) * n) != 0;
}

const char* SourceTag(int s) {
    switch (s) {
        case AWT_SRC_TWEAKS: return "ini";
        case AWT_SRC_PRESET: return "preset";
        case AWT_SRC_OVERLAY: return "edited";
        default: return nullptr;
    }
}

ImVec4 SourceColor(int s) {
    switch (s) {
        case AWT_SRC_TWEAKS: return ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
        case AWT_SRC_PRESET: return ImVec4(0.75f, 0.6f, 1.0f, 1.0f);
        case AWT_SRC_OVERLAY: return ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
        default: return ImVec4(1, 1, 1, 1);
    }
}

void DrawItem(const AwtInfo& t, const std::string& label) {
    ImGui::PushID(t.name);
    float v[4];
    memcpy(v, t.value, sizeof(v));
    bool edited = false;

    const float resetWidth = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(AvailWidth() * 0.55f);
    if (t.type == AWT_BOOL) {
        bool b = v[0] != 0.0f;
        if (ImGui::Checkbox("##v", &b)) {
            v[0] = b ? 1.0f : 0.0f;
            edited = true;
        }
    } else if (t.type == AWT_FLOAT) {
        // Ctrl+click types a value, which may go past the engine's slider range.
        if (t.min < t.max)
            edited = ImGui::SliderFloat("##v", &v[0], t.min, t.max, "%.4g", ImGuiSliderFlags_None);
        else
            edited = ImGui::DragFloat("##v", &v[0], 0.01f, 0.0f, 0.0f, "%.4g", ImGuiSliderFlags_None);
    } else if (t.type == AWT_COLOR) {
        edited = ImGui::ColorEdit4("##v", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    } else if (t.type == AWT_LIST) {
        // Combo wants the options separated by '\0' and ended by an extra '\0'.
        char opts[sizeof(t.items) + 1];
        size_t n = strlen(t.items);
        memcpy(opts, t.items, n);
        for (size_t i = 0; i < n; ++i)
            if (opts[i] == '\n') opts[i] = '\0';
        opts[n] = '\0';
        int sel = (int)v[0];
        if (ImGui::Combo("##v", &sel, opts, 10)) {
            v[0] = (float)sel;
            edited = true;
        }
    } else {
        ImGui::TextDisabled("(type %d, read-only)", t.type);
    }
    if (edited) g_api.set(t.name, v);

    ImGui::SameLine();
    if (const char* tag = SourceTag(t.source)) {
        ImGui::TextColored(SourceColor(t.source), "%s", label.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set by %s", tag);
    } else {
        ImGui::TextUnformatted(label.c_str());
    }
    if (ImGui::IsItemHovered() && Editable(t)) {
        if (t.type == AWT_BOOL) ImGui::SetTooltip("%s\nstock: %s", t.name, t.original[0] ? "true" : "false");
        else if (t.type == AWT_FLOAT) ImGui::SetTooltip("%s\nstock: %g  (range %g..%g)", t.name, t.original[0], t.min, t.max);
        else if (t.type == AWT_LIST)
            ImGui::SetTooltip("%s\nstock: %s", t.name, ListOption(t, (int)t.original[0]).c_str());
        else ImGui::SetTooltip("%s\nstock: %g, %g, %g, %g", t.name, t.original[0], t.original[1], t.original[2], t.original[3]);
    }
    if (t.source == AWT_SRC_OVERLAY) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, AvailWidth() - resetWidth));
        if (ImGui::Button("x", ImVec2(resetWidth, 0))) g_api.reset(t.name);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo this edit (back to ini/preset/stock)");
    }
    ImGui::PopID();
}

std::vector<AwtPreset> g_presets(32);
std::string g_lastStartup;

void DrawDescription(const char* text) {
    if (!text[0]) return;
    ImGui::Indent(ImGui::GetFrameHeight() + 4.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::Unindent(ImGui::GetFrameHeight() + 4.0f);
}

AwtBenchmark g_bench{};

// "142 fps (-9%)" next to a Quality choice, once the benchmark has measured it.
void DrawFps(const char* name) {
    float base = 0, mine = 0;
    for (int i = 0; i < g_bench.resultCount; ++i) {
        if (!strcmp(g_bench.results[i].name, "Stock")) base = g_bench.results[i].fps;
        if (!strcmp(g_bench.results[i].name, name)) mine = g_bench.results[i].fps;
    }
    if (mine <= 0) return;
    ImGui::SameLine();
    if (base > 0 && strcmp(name, "Stock"))
        ImGui::TextDisabled("%.0f fps (%+.0f%%)", mine, (mine / base - 1.0f) * 100.0f);
    else
        ImGui::TextDisabled("%.0f fps", mine);
}

// The everyday part: mod switch and presets. Groups with several presets are
// exclusive (radio buttons plus a "none" choice); single presets are checkboxes.
void DrawSimple(const AwtStatus& st) {
    ImGui::TextDisabled("AWTweaks " AWT_VERSION_STRING);
    bool on = !st.modOff;
    if (ImGui::Checkbox("Mod enabled", &on)) g_api.setModOn(on ? 1 : 0);
    ImGui::SameLine();
    ImGui::TextDisabled("(F10 compares with stock)");

    g_api.getBench(&g_bench);
    int pn = g_api.presets(g_presets.data(), (int)g_presets.size());
    if (pn > (int)g_presets.size()) {
        g_presets.resize(pn + 8);
        pn = g_api.presets(g_presets.data(), (int)g_presets.size());
    }
    // Groups in order of first appearance; "" (ungrouped) goes last as "Extras".
    std::vector<std::string> order;
    for (int i = 0; i < pn; ++i) {
        std::string g = g_presets[i].group;
        if (!g.empty() && std::find(order.begin(), order.end(), g) == order.end()) order.push_back(g);
    }
    order.push_back("");

    ImGui::BeginDisabled(st.modOff);
    for (const std::string& group : order) {
        std::vector<int> members;
        for (int i = 0; i < pn; ++i)
            if (group == g_presets[i].group) members.push_back(i);
        if (members.empty()) continue;
        ImGui::SeparatorText(group.empty() ? "Extras" : group.c_str());
        ImGui::PushID(group.c_str());
        if (!group.empty() && members.size() > 1) {
            bool any = false;
            for (int i : members) any |= g_presets[i].active != 0;
            const bool quality = Lower(group) == "quality";
            if (ImGui::RadioButton(quality ? "Stock (game's own settings)" : "None", !any))
                for (int i : members) g_api.setPreset(g_presets[i].name, 0);
            if (quality) DrawFps("Stock");
            for (int i : members) {
                if (ImGui::RadioButton(g_presets[i].name, g_presets[i].active != 0))
                    g_api.setPreset(g_presets[i].name, 1);
                if (quality) DrawFps(g_presets[i].name);
                DrawDescription(g_presets[i].description);
            }
            if (quality) {
                if (g_bench.running) {
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Measuring %s (%d of %d)... hold still",
                                       g_bench.current, g_bench.stage + 1, g_bench.stages);
                } else {
                    ImGui::BeginDisabled(!g_bench.available);
                    if (ImGui::Button("Measure FPS of each (about 20 s)")) g_api.startBench();
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(g_bench.available
                                              ? "Runs Stock and each Quality preset for 7 s and shows the average fps.\n"
                                                "Keep the view still (pause with F8, or freeze the free camera)\n"
                                                "so the numbers compare like for like."
                                              : "Needs [Shadows] AtlasScale=2 (frames are counted in its hook).");
                }
            }
        } else {
            for (int i : members) {
                bool act = g_presets[i].active != 0;
                if (ImGui::Checkbox(g_presets[i].name, &act)) g_api.setPreset(g_presets[i].name, act ? 1 : 0);
                DrawDescription(g_presets[i].description);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (ImGui::Button("Use these presets at startup")) {
        g_lastStartup = g_api.saveStartup() ? "Saved to awtweaks.ini" : "Save failed - see awtweaks.log";
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Writes Preset=%s into awtweaks.ini", st.preset[0] ? st.preset : "None");
    if (!g_lastStartup.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_lastStartup.c_str());
    }
}

// Photo mode: free camera, pause / frame step, game speed and time of day.
void DrawPhoto() {
    AwtPhoto ph{};
    g_api.photo(&ph);
    ImGui::SeparatorText("Photo mode");
    if (!ph.freeCamAvailable && !ph.photoAvailable) {
        ImGui::TextDisabled("Off: set Enabled=true under [FreeCamera] / [PhotoMode] in awtweaks.ini and restart.");
        return;
    }
    ImGui::PushID("photo");
    if (ph.freeCamAvailable) {
        const char* cam = ph.cameraMode == 1 || ph.cameraMode == 6 ? "Free camera: flying  (click: freeze it)"
                          : ph.cameraMode == 2                    ? "Free camera: frozen, Alan playable  (click: off)"
                          : ph.cameraMode == 0                    ? "Free camera: off  (click: fly)"
                                                                  : "Free camera: unavailable during this scene";
        ImGui::BeginDisabled(ph.cameraMode > 2 && ph.cameraMode != 6);
        if (ImGui::Button(cam)) g_api.photoAction(AWT_PHOTO_FREECAM);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fly: move keys / left stick, mouse / right stick to look,\n"
                              "Space up, Left Ctrl down, Left Shift faster, Left Alt slower.");
    }
    if (ph.photoAvailable) {
        if (ImGui::Button(ph.paused ? "Resume" : "Pause")) g_api.photoAction(AWT_PHOTO_PAUSE);
        ImGui::SameLine();
        ImGui::BeginDisabled(!ph.paused);
        if (ImGui::Button("Step one frame")) g_api.photoAction(AWT_PHOTO_STEP);
        ImGui::EndDisabled();

        float ts = ph.timeScale;
        ImGui::SetNextItemWidth(AvailWidth() * 0.55f);
        if (ImGui::SliderFloat("Game speed", &ts, 0.01f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
            g_api.setTimeScale(ts);
        ImGui::SameLine();
        if (ImGui::Button("1x")) g_api.setTimeScale(1.0f);

        ImGui::TextUnformatted("Time of day");
        ImGui::SameLine();
        if (ImGui::Button("-1 h")) g_api.photoAction(AWT_PHOTO_TIME_MINUS_HOUR);
        ImGui::SameLine();
        if (ImGui::Button("-1 min")) g_api.photoAction(AWT_PHOTO_TIME_MINUS_MINUTE);
        ImGui::SameLine();
        if (ImGui::Button("+1 min")) g_api.photoAction(AWT_PHOTO_TIME_PLUS_MINUTE);
        ImGui::SameLine();
        if (ImGui::Button("+1 h")) g_api.photoAction(AWT_PHOTO_TIME_PLUS_HOUR);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Some scenes script their own lighting and may ignore this.");
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Keys: %s", ph.keys);
    ImGui::PopTextWrapPos();
    ImGui::PopID();
}

// Expert part: every engine tweakable, collapsed by default.
void DrawAdvanced(const AwtStatus& st, int n) {
    char header[128];
    if (st.overlayEdits)
        snprintf(header, sizeof(header), "Advanced: individual engine settings  (%d unsaved edit%s)###adv",
                 st.overlayEdits, st.overlayEdits == 1 ? "" : "s");
    else
        snprintf(header, sizeof(header), "Advanced: individual engine settings###adv");
    if (!ImGui::CollapsingHeader(header)) return;

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Raw engine values (%d found so far; more appear as levels load). Edits apply at once "
                        "and override the presets above until you save or undo them. Hover a name for its stock "
                        "value; Ctrl+click a slider to type any value. Name colours: ini / preset / edited.", n);
    ImGui::PopTextWrapPos();

    ImGui::BeginDisabled(st.overlayEdits == 0);
    char saveLabel[64];
    snprintf(saveLabel, sizeof(saveLabel), "Save %d edit(s) to awtweaks.ini", st.overlayEdits);
    if (ImGui::Button(saveLabel)) {
        int r = g_api.save();
        g_lastSave = r < 0 ? "Save failed - see awtweaks.log" : "Saved " + std::to_string(r) + " value(s)";
    }
    ImGui::SameLine();
    if (ImGui::Button("Undo all edits")) g_api.reset(nullptr);
    ImGui::EndDisabled();
    if (!g_lastSave.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_lastSave.c_str());
    }

    ImGui::SetNextItemWidth(AvailWidth() * 0.55f);
    ImGui::InputTextWithHint("##filter", "Filter (e.g. shadow, hbao, fog)", g_filter, sizeof(g_filter));
    ImGui::SameLine();
    ImGui::Checkbox("Only changed", &g_onlyChanged);
    ImGui::Separator();

    // Group by the "Category:" prefix, sorted by name.
    const std::string filter = Lower(g_filter);
    std::map<std::string, std::vector<int>> groups;
    for (int i = 0; i < n; ++i) {
        const AwtInfo& t = g_items[i];
        if (g_onlyChanged && !Changed(t)) continue;
        if (!filter.empty() && Lower(t.name).find(filter) == std::string::npos) continue;
        const char* colon = strchr(t.name, ':');
        groups[colon ? std::string(t.name, colon - t.name) : std::string("(other)")].push_back(i);
    }

    const bool forceOpen = !filter.empty() || g_onlyChanged;
    ImGui::Indent(8.0f);
    for (auto& [group, idx] : groups) {
        int changed = 0;
        for (int i : idx) changed += g_items[i].source != AWT_SRC_NONE;
        char h[160];
        if (changed)
            snprintf(h, sizeof(h), "%s  (%zu, %d set)###g_%s", group.c_str(), idx.size(), changed, group.c_str());
        else
            snprintf(h, sizeof(h), "%s  (%zu)###g_%s", group.c_str(), idx.size(), group.c_str());
        if (forceOpen) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (!ImGui::CollapsingHeader(h)) continue;
        std::sort(idx.begin(), idx.end(), [](int a, int b) { return strcmp(g_items[a].name, g_items[b].name) < 0; });
        for (int i : idx) {
            const AwtInfo& t = g_items[i];
            const char* colon = strchr(t.name, ':');
            DrawItem(t, colon ? colon + 1 : t.name);
        }
    }
    ImGui::Unindent(8.0f);
}

// Sun/moon shadow shader: soft (sun-size) and contact shadows, when [Shaders]
// ShadowShader=Enhanced has the add-on replacing the game's shader.
void DrawShadowFx(const AwtStatus& st) {
    shader_replace::Stats s = shader_replace::GetStats(shader_replace::Family::Shadow);
    ImGui::SeparatorText("Sun and moon shadows");
    if (s.mode == shader_replace::Mode::Original) {
        ImGui::TextDisabled("Soft and contact shadows: set [Shaders] ShadowShader=Enhanced in "
                            "awtweaks.ini and restart the game.");
        return;
    }
    if (s.mode == shader_replace::Mode::Debug)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Debug test pattern on ([Shaders] ShadowShader=Debug)");
    ImGui::BeginDisabled(st.modOff || s.mode != shader_replace::Mode::Enhanced);
    float sun = st.sunSize, contact = st.contactShadows;
    ImGui::SetNextItemWidth(AvailWidth() * 0.6f);
    if (ImGui::SliderFloat("Sun size", &sun, 0.0f, 3.0f, sun > 0.0f ? "%.1f deg" : "off (game's filter)",
                           ImGuiSliderFlags_None))
        g_api.set("AWTweaks:Sun Size", &sun);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shadows soften the farther they fall from what casts them, like real sunlight.\n"
                          "The real sun is 0.5 degrees; larger looks softer. Ctrl+click to type a value.");
    ImGui::SetNextItemWidth(AvailWidth() * 0.6f);
    if (ImGui::SliderFloat("Contact shadows", &contact, 0.0f, 1.0f, contact > 0.0f ? "%.2f m" : "off",
                           ImGuiSliderFlags_None))
        g_api.set("AWTweaks:Contact Shadows", &contact);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Small sharp shadows where objects meet the ground and walls, traced on screen\n"
                          "up to this distance towards the sun. Fades out beyond 25-50 m.");
    ImGui::EndDisabled();
    ImGui::TextDisabled("%d of %d shadow shaders replaced (%d pipelines)%s", s.variantsReplaced, s.variantsTotal,
                        s.pipelinesReplaced, s.pipelinesMissed ? ", some missed - see ReShade.log" : "");

}

// Ambient occlusion: GTAO in place of the game's HBAO+, when [Shaders] AOShader=Enhanced.
void DrawAoFx(const AwtStatus& st) {
    shader_replace::Stats s = shader_replace::GetStats(shader_replace::Family::AO);
    ImGui::SeparatorText("Ambient occlusion");
    if (s.mode == shader_replace::Mode::Original) {
        ImGui::TextDisabled("GTAO: set [Shaders] AOShader=Enhanced in awtweaks.ini and restart the game.");
        return;
    }
    if (s.mode == shader_replace::Mode::Debug)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Debug test pattern on ([Shaders] AOShader=Debug)");
    ImGui::BeginDisabled(st.modOff);
    float gtao = st.gtao;
    ImGui::SetNextItemWidth(AvailWidth() * 0.6f);
    if (ImGui::SliderFloat("GTAO", &gtao, 0.0f, 3.0f, gtao >= 0.05f ? "strength %.1f" : "off (game's HBAO+)",
                           ImGuiSliderFlags_None))
        g_api.set("AWTweaks:GTAO", &gtao);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ground-truth ambient occlusion: darkens creases and contact points the way\n"
                          "they really are, without HBAO+'s grey wash on open surfaces or halos\n"
                          "behind thin objects. 1 matches HBAO+'s darkest shading.");
    ImGui::EndDisabled();
    ImGui::TextDisabled("%d of %d AO shaders replaced (%d pipelines)%s", s.variantsReplaced, s.variantsTotal,
                        s.pipelinesReplaced, s.pipelinesMissed ? ", some missed - see ReShade.log" : "");
}

void DrawOverlay(reshade::api::effect_runtime*) {
    if (!ResolveApi()) {
        ImGui::TextWrapped("AWTweaks' xinput1_4.dll isn't loaded, so there is nothing to edit. "
                           "Install it next to Game_f_x64_EOS.exe.");
        return;
    }
    AwtStatus st{};
    g_api.status(&st);
    if (st.apiVersion != AWT_API_VERSION) {
        ImGui::TextWrapped("Version mismatch: xinput1_4.dll speaks API %d, this add-on %d. Update both.",
                           st.apiVersion, AWT_API_VERSION);
        return;
    }
    int n = g_api.snapshot(g_items.data(), (int)g_items.size());
    if (n > (int)g_items.size()) {
        g_items.resize(n + 256);
        n = g_api.snapshot(g_items.data(), (int)g_items.size());
    }

    DrawSimple(st);
    ImGui::Spacing();
    DrawPhoto();
    ImGui::Spacing();
    DrawShadowFx(st);
    ImGui::Spacing();
    DrawAoFx(st);
    ImGui::Spacing();
    DrawAdvanced(st, n);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            reshade::register_overlay("AWTweaks", &DrawOverlay);
            shader_replace::Install();
            break;
        case DLL_PROCESS_DETACH:
            shader_replace::Uninstall();
            reshade::unregister_overlay("AWTweaks", &DrawOverlay);
            reshade::unregister_addon(module);
            break;
    }
    return TRUE;
}
