// Diagnostic: which of the game's shaders actually draw. [Debug] WatchShaders lists
// shaderbin names (e.g. "SSSSS,Skin_d3t,Eye"); every pixel shader in those files is
// matched by DXBC digest when the game creates a pipeline, and draws with it are
// counted and logged every few seconds. Off (no events registered) unless set.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <reshade.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "shader_watch.h"

namespace shader_watch {
namespace {

using namespace reshade::api;

struct Watched {
    std::string label;  // "<file> #<blob index>", numbered like the extracted blobs
    std::atomic<uint64_t> draws{0};
    uint64_t reported = 0;
    bool created = false;
};

std::deque<Watched> g_watched;                       // fixed after Install
std::unordered_map<std::string, size_t> g_byDigest;  // digest + size -> g_watched index
std::unordered_map<uint64_t, size_t> g_pipelines;    // pipeline handle -> g_watched index
std::unordered_map<command_list*, int> g_current;    // bound pixel pipeline, -1 = not watched
SRWLOCK g_lock = SRWLOCK_INIT;
ULONGLONG g_lastReport = 0;
bool g_registered = false;

void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    reshade::log::message(reshade::log::level::info, buf);
}

std::string Key(const void* code, size_t size) {
    std::string k(static_cast<const char*>(code) + 4, 16);
    k.append(reinterpret_cast<const char*>(&size), sizeof(size));
    return k;
}

std::wstring GameDir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring dir(path, n);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

// Adds every pixel shader in one shaderbin. Returns how many.
int AddFile(const std::wstring& dir, const std::string& name) {
    std::wstring path = dir + L"shaders\\build\\pc_DXIL\\" + std::wstring(name.begin(), name.end()) + L".shaderbin";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return -1;
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    int blob = 0, added = 0;
    for (size_t i = 0; i + 32 <= d.size();) {
        if (memcmp(&d[i], "DXBC", 4) != 0) {
            ++i;
            continue;
        }
        uint32_t size, parts;
        memcpy(&size, &d[i + 24], 4);
        memcpy(&parts, &d[i + 28], 4);
        if (size > d.size() - i || parts > 64 || 32 + 4 * parts > size) {
            i += 4;
            continue;
        }
        const uint8_t* b = &d[i];
        bool pixel = false;
        for (uint32_t k = 0; k < parts; ++k) {
            uint32_t off;
            memcpy(&off, b + 32 + 4 * k, 4);
            if (off + 12 <= size && memcmp(b + off, "DXIL", 4) == 0) {
                uint16_t kind;  // high half of the program version; 0 = pixel shader
                memcpy(&kind, b + off + 10, 2);
                pixel = kind == 0;
            }
        }
        if (pixel) {
            std::string key = Key(b, size);
            if (!g_byDigest.count(key)) {
                char label[96];
                snprintf(label, sizeof(label), "%s #%02d", name.c_str(), blob);
                g_watched.emplace_back().label = label;
                g_byDigest[key] = g_watched.size() - 1;
                ++added;
            }
        }
        ++blob;
        i += size;
    }
    return added;
}

void OnInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline p) {
    for (uint32_t i = 0; i < count; ++i) {
        if (subobjects[i].type != pipeline_subobject_type::pixel_shader || !subobjects[i].data) continue;
        auto* desc = static_cast<const shader_desc*>(subobjects[i].data);
        if (!desc->code || desc->code_size < 20) continue;
        auto it = g_byDigest.find(Key(desc->code, desc->code_size));
        if (it == g_byDigest.end()) continue;
        AcquireSRWLockExclusive(&g_lock);
        g_pipelines[p.handle] = it->second;
        Watched& w = g_watched[it->second];
        bool first = !w.created;
        w.created = true;
        ReleaseSRWLockExclusive(&g_lock);
        if (first) Log("AWTweaks watch: pipeline created with %s", w.label.c_str());
    }
}

void OnDestroyPipeline(device*, pipeline p) {
    AcquireSRWLockExclusive(&g_lock);
    g_pipelines.erase(p.handle);
    ReleaseSRWLockExclusive(&g_lock);
}

void OnBindPipeline(command_list* cmd, pipeline_stage stages, pipeline p) {
    if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::pixel_shader)) == 0) return;
    AcquireSRWLockExclusive(&g_lock);
    auto it = g_pipelines.find(p.handle);
    g_current[cmd] = it == g_pipelines.end() ? -1 : (int)it->second;
    ReleaseSRWLockExclusive(&g_lock);
}

void Count(command_list* cmd, uint64_t n) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_current.find(cmd);
    int w = it == g_current.end() ? -1 : it->second;
    ReleaseSRWLockShared(&g_lock);
    if (w >= 0) g_watched[w].draws += n;
}

bool OnDraw(command_list* cmd, uint32_t, uint32_t, uint32_t, uint32_t) {
    Count(cmd, 1);
    return false;
}
bool OnDrawIndexed(command_list* cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
    Count(cmd, 1);
    return false;
}
bool OnDrawIndirect(command_list* cmd, indirect_command type, resource, uint64_t, uint32_t drawCount, uint32_t) {
    if (type != indirect_command::dispatch) Count(cmd, drawCount);
    return false;
}

// Every 5 s: draws per watched shader since the last report, 8 to a log line.
void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastReport < 5000) return;
    g_lastReport = now;
    std::string line;
    int shown = 0;
    for (Watched& w : g_watched) {
        uint64_t total = w.draws.load();
        if (total == w.reported) continue;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s%s: %llu", shown % 8 ? ", " : "", w.label.c_str(),
                 (unsigned long long)(total - w.reported));
        line += buf;
        w.reported = total;
        if (++shown % 8 == 0) {
            Log("AWTweaks watch: draws in the last 5 s: %s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) Log("AWTweaks watch: draws in the last 5 s: %s", line.c_str());
}

}  // namespace

void Install() {
    std::wstring dir = GameDir();
    wchar_t v[512] = {};
    GetPrivateProfileStringW(L"Debug", L"WatchShaders", L"", v, 512, (dir + L"awtweaks.ini").c_str());
    std::string list;
    for (wchar_t* c = v; *c; ++c) list += (char)*c;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        std::string name = list.substr(pos, end - pos);
        while (!name.empty() && name.front() == ' ') name.erase(0, 1);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        pos = end + 1;
        if (name.empty()) continue;
        int n = AddFile(dir, name);
        if (n < 0) Log("AWTweaks watch: %s.shaderbin not found", name.c_str());
        else Log("AWTweaks watch: %s: %d distinct pixel shaders", name.c_str(), n);
    }
    if (g_watched.empty()) return;
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::draw>(&OnDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnDrawIndirect);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
    g_registered = true;
}

void Uninstall() {
    if (!g_registered) return;
    reshade::unregister_event<reshade::addon_event::init_pipeline>(&OnInitPipeline);
    reshade::unregister_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::unregister_event<reshade::addon_event::draw>(&OnDraw);
    reshade::unregister_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
    reshade::unregister_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnDrawIndirect);
    reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
    g_registered = false;
}

}  // namespace shader_watch
