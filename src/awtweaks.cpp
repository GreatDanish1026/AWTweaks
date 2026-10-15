// AWTweaks - Alan Wake Remastered engine tweakable override.
//
// Loads as xinput1_4.dll from the game folder (the game imports it by ordinal),
// forwards all XInput calls to the system DLL, and runs a background thread that
// writes values from awtweaks.ini into Remedy's tweakable-variable registry.
//
// Engine layout (Game_f_x64_EOS.exe, build 34885), found by RE of the tweakable ctor:
//   registry : Tweakable* g_tweakables[0x500];  int g_tweakableCount;
//   Tweakable: +0x00 const char* name   (inline buffer at +0x28, 128 bytes)
//              +0xA8 int type          (0 = bool, 1 = float, 3 = color/vec4, 2/4/5 = other)
//              +0xB0 value             (bool: byte; float: value, +0xB4 min, +0xB8 max;
//                                       color: float4)
// The registry is located at runtime via a byte signature in the ctor, so it
// should survive small patches as long as the ctor code is unchanged.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "awtweaks_api.h"
#include "version.h"

namespace {

// ---------------------------------------------------------------------------
// XInput forwarding

HMODULE g_realXInput = nullptr;
INIT_ONCE g_xinputOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadRealXInput(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 16 >= MAX_PATH) return TRUE;
    wcscat_s(path, L"\\xinput1_4.dll");
    g_realXInput = LoadLibraryW(path);
    return TRUE;
}

using Fwd = uintptr_t(WINAPI*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

HMODULE g_self = nullptr;

// Walks the export table ourselves instead of calling GetProcAddress: overlays
// (Steam, Epic) hook GetProcAddress for XInput and hand back a detour that calls
// the game's import - i.e. this DLL - which recursed until the stack overflowed.
void* FindExport(HMODULE mod, const char* nameOrOrdinal) {
    auto base = reinterpret_cast<const uint8_t*>(mod);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto funcs = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
    DWORD index = MAXDWORD;
    if (IS_INTRESOURCE(nameOrOrdinal)) {
        index = (DWORD)(uintptr_t)nameOrOrdinal - exp->Base;
    } else {
        auto names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
        auto ords = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);
        for (DWORD i = 0; i < exp->NumberOfNames; ++i)
            if (!strcmp(reinterpret_cast<const char*>(base + names[i]), nameOrOrdinal)) {
                index = ords[i];
                break;
            }
    }
    if (index >= exp->NumberOfFunctions || !funcs[index]) return nullptr;
    DWORD rva = funcs[index];
    if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) return nullptr;  // forwarder
    return const_cast<uint8_t*>(base + rva);
}

Fwd Resolve(const char* nameOrOrdinal, Fwd* cache) {
    if (*cache) return *cache;
    InitOnceExecuteOnce(&g_xinputOnce, LoadRealXInput, nullptr, nullptr);
    if (!g_realXInput || g_realXInput == g_self) return nullptr;
    *cache = reinterpret_cast<Fwd>(FindExport(g_realXInput, nameOrOrdinal));
    return *cache;
}

// Belt and braces: if anything still routes a call back into us, fail the call
// instead of recursing.
thread_local int t_depth = 0;

// ---------------------------------------------------------------------------
// Logging / paths

std::wstring g_dir;  // folder holding this DLL, with trailing backslash
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------
// Memory helpers

bool Readable(const void* p, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    auto end = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const uint8_t*>(p) + len <= end;
}

// Pattern with -1 as wildcard.
const uint8_t* FindPattern(const uint8_t* begin, size_t size, const std::vector<int>& pat) {
    if (pat.empty() || size < pat.size()) return nullptr;
    for (size_t i = 0; i + pat.size() <= size; ++i) {
        size_t j = 0;
        for (; j < pat.size(); ++j)
            if (pat[j] >= 0 && begin[i + j] != pat[j]) break;
        if (j == pat.size()) return begin + i;
    }
    return nullptr;
}

const uint8_t* RipTarget(const uint8_t* insn, int dispOffset, int insnLen) {
    int32_t disp;
    memcpy(&disp, insn + dispOffset, 4);
    return insn + insnLen + disp;
}

// ---------------------------------------------------------------------------
// Engine registry

constexpr int kMaxTweakables = 0x500;
constexpr size_t kOffName = 0x00;
constexpr size_t kOffType = 0xA8;
constexpr size_t kOffValue = 0xB0;
constexpr size_t kObjSize = 0xC0;  // enough for float4 value

enum TweakType : int { kBool = 0, kFloat = 1, kColor = 3, kList = 5 };

// TweakableList (type 5): a vector of engine strings plus the selected index.
//   +0xB0 items (0xA8-byte strings, text pointer first)  +0xB8 count  +0xD0 int selected
constexpr size_t kOffListItems = 0xB0, kOffListCount = 0xB8, kOffListIndex = 0xD0, kListItemStride = 0xA8;

int ListCount(const uint8_t* obj) {
    if (!Readable(obj + kOffListItems, 0x28)) return 0;
    size_t n = *reinterpret_cast<const size_t*>(obj + kOffListCount);
    return n <= 64 ? (int)n : 0;
}

const char* ListItem(const uint8_t* obj, int i) {
    if (i < 0 || i >= ListCount(obj)) return nullptr;
    auto items = *reinterpret_cast<const uint8_t* const*>(obj + kOffListItems);
    if (!Readable(items + (size_t)i * kListItemStride, 8)) return nullptr;
    const char* text = *reinterpret_cast<const char* const*>(items + (size_t)i * kListItemStride);
    return Readable(text, 1) ? text : nullptr;
}

int& ListIndex(uint8_t* obj) { return *reinterpret_cast<int*>(obj + kOffListIndex); }

uint8_t** g_registry = nullptr;
int* g_registryCount = nullptr;

uint8_t* GameBase() { return reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)); }

unsigned long long Rva(const void* p) {
    return (unsigned long long)(static_cast<const uint8_t*>(p) - GameBase());
}

// Searches the game's sections whose characteristics include `flags`.
uint8_t* FindInGame(const std::vector<int>& pat, DWORD flags = IMAGE_SCN_MEM_EXECUTE) {
    uint8_t* base = GameBase();
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((sec->Characteristics & flags) != flags) continue;
        if (auto hit = FindPattern(base + sec->VirtualAddress, sec->Misc.VirtualSize, pat))
            return const_cast<uint8_t*>(hit);
    }
    return nullptr;
}

bool LocateRegistry() {
    // movsxd rcx,[count]; cmp ecx,500h; jge +x; lea rdx,[array]; mov [rdx+rcx*8],rdi; inc ecx; mov [count],ecx
    const uint8_t* hit = FindInGame({0x48, 0x63, 0x0D, -1,   -1,   -1,   -1,   0x81, 0xF9, 0x00,
                                     0x05, 0x00, 0x00, 0x7D, -1,   0x48, 0x8D, 0x15, -1,   -1,
                                     -1,   -1,   0x48, 0x89, 0x3C, 0xCA, 0xFF, 0xC1, 0x89, 0x0D});
    if (!hit) return false;
    g_registryCount = (int*)RipTarget(hit, 3, 7);
    g_registry = (uint8_t**)RipTarget(hit + 15, 3, 7);
    Log("Registry found: ctor sig at +0x%llx, array +0x%llx, count +0x%llx", Rva(hit),
        Rva(g_registry), Rva(g_registryCount));
    return true;
}

// ---------------------------------------------------------------------------
// Shadow atlas diagnostics
//
// rend::ShadowMapAllocator (vtable slot 1) = Region* allocate(this, int w, int h, bool flag).
// The engine doubles w/h there during cinematics ("Cinematic Render:Double Shadow Map Res").
// The atlas itself is this+0x10 x this+0x14 (4096x2048, doubled to 8192x4096 in large mode).
// With [Debug] LogShadowAllocs we wrap that slot and log what is requested, by whom.

using AllocateFn = void*(__fastcall*)(void*, int, int, uint8_t);
AllocateFn g_origAllocate = nullptr;

struct AllocKey {
    int w, h, flag;
    unsigned long long caller;
    bool operator==(const AllocKey& o) const {
        return w == o.w && h == o.h && flag == o.flag && caller == o.caller;
    }
};
struct AllocKeyHash {
    size_t operator()(const AllocKey& k) const {
        return std::hash<unsigned long long>()(k.caller * 31 + k.w * 7919 + k.h * 13 + k.flag);
    }
};
struct AllocStat {
    unsigned long long calls = 0, failures = 0;
};

SRWLOCK g_allocLock = SRWLOCK_INIT;
std::unordered_map<AllocKey, AllocStat, AllocKeyHash> g_allocStats;
bool g_allocDirty = false;
int g_atlasW = 0, g_atlasH = 0, g_atlasLarge = -1;

// [Shadows] LightShadowScale: multiplies spot/point-light shadow requests. Lights
// are the only callers that pass flag=1 ("resizable": the packer halves them on
// overflow), so the renderer already copes with a region size that differs from
// the light's authored size. Updated live from the ini.
volatile LONG g_lightShadowScale = 1;
volatile LONG g_diagAllocate = 0;
constexpr int kMaxLightShadow = 4096;

void* __fastcall AllocateHook(void* self, int w, int h, uint8_t flag) {
    const LONG scale = g_lightShadowScale;
    if (flag && scale > 1) {
        w = (std::min)(w * (int)scale, kMaxLightShadow);
        h = (std::min)(h * (int)scale, kMaxLightShadow);
    }
    void* region = g_origAllocate(self, w, h, flag);
    if (!g_diagAllocate) return region;
    AllocKey key{w, h, flag, Rva(__builtin_return_address(0))};
    AcquireSRWLockExclusive(&g_allocLock);
    auto& s = g_allocStats[key];
    ++s.calls;
    if (!region) ++s.failures;
    g_allocDirty = true;
    auto p = static_cast<uint8_t*>(self);
    g_atlasW = *reinterpret_cast<int*>(p + 0x10);
    g_atlasH = *reinterpret_cast<int*>(p + 0x14);
    g_atlasLarge = p[0x18];
    ReleaseSRWLockExclusive(&g_allocLock);
    return region;
}

// ShadowMapAllocator vtable slot 3 = repack(this): packs every live region into
// the atlas. The packer (this+0x30: +0 W, +4 H, +0x10 entries, +0x18 count;
// entry 0x14 bytes: +8 w, +0xC h, +0x10 resizable) sums power-of-two areas and,
// while that exceeds W*H, halves resizable entries (lights) round-robin.
// Afterwards each region (this+0x58, 256 pointers; +0x30 handle >= 0 when live)
// holds its placement at +0x34 x, +0x38 y, +0x3C w, +0x40 h.
using RepackFn = void(__fastcall*)(void*);
RepackFn g_origRepack = nullptr;

struct RepackStats {
    unsigned long long frames = 0, overflowFrames = 0, halvedEntries = 0, badPlacements = 0;
    unsigned long long maxEntries = 0, maxLive = 0;
    double maxFill = 0;  // pow2 area / atlas area, before shrinking
    int lastBadX = 0, lastBadY = 0, lastBadW = 0, lastBadH = 0;
};
RepackStats g_repack;

unsigned long long Pow2Area(int w, int h) {
    unsigned long long s = 1;
    while (s < (unsigned long long)(std::max)(w, h)) s <<= 1;
    return s * s;
}

bool g_diagRepack = false;    // collect RepackStats
bool g_largeSlotFix = false;   // use our packer when a region needs a >2048 slot
unsigned long long g_ourPacks = 0, g_ourPackFailures = 0;

int Pow2Side(int w, int h) {
    int s = 1;
    while (s < (std::max)(w, h)) s <<= 1;
    return s;
}

// Replacement for the engine's packer. The original walks the atlas in fixed
// 2048x2048 cells and never places anything whose power-of-two side is larger,
// leaving it - and every smaller region queued behind it - at (0,0). Cascades
// above 2048 therefore overlapped each other: the flicker at 4096.
// Same overflow rule as the original (halve resizable entries until the
// power-of-two areas fit), then a quadtree of power-of-two squares, largest first.
// Returns false if something could not be placed (left at 0,0 like the original).
bool PackLarge(uint8_t* packer) {
    const int W = *reinterpret_cast<int*>(packer), H = *reinterpret_cast<int*>(packer + 4);
    uint8_t* entries = *reinterpret_cast<uint8_t**>(packer + 0x10);
    const size_t n = *reinterpret_cast<size_t*>(packer + 0x18);
    auto E = [&](size_t i) { return reinterpret_cast<int*>(entries + i * 0x14); };  // x, y, w, h, flag

    unsigned long long sum = 0;
    for (size_t i = 0; i < n; ++i) sum += Pow2Area(E(i)[2], E(i)[3]);
    const unsigned long long cap = (unsigned long long)W * H;
    for (size_t i = 0, sinceChange = 0; sum > cap && n && sinceChange < n; i = (i + 1) % n) {
        int* e = E(i);
        if ((e[4] & 0xFF) && e[2] > 32 && e[3] > 32) {
            unsigned long long before = Pow2Area(e[2], e[3]);
            e[2] /= 2;
            e[3] /= 2;
            sum = sum - before + Pow2Area(e[2], e[3]);
            sinceChange = 0;
        } else {
            ++sinceChange;
        }
    }

    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return Pow2Side(E(a)[2], E(a)[3]) > Pow2Side(E(b)[2], E(b)[3]);
    });

    // Free squares bucketed by log2(side).
    std::vector<std::vector<std::pair<int, int>>> freeBy(16);
    auto lg = [](int s) {
        int l = 0;
        while ((1 << l) < s) ++l;
        return l;
    };
    int root = 1;
    while (root * 2 <= (std::min)(W, H)) root *= 2;
    for (int y = 0; y + root <= H; y += root)
        for (int x = 0; x + root <= W; x += root) freeBy[lg(root)].push_back({x, y});
    // Keep the first-pushed square on top so placement starts at the top-left.
    for (auto& v : freeBy) std::reverse(v.begin(), v.end());

    bool ok = true;
    for (size_t i : order) {
        int* e = E(i);
        const int s = Pow2Side(e[2], e[3]), want = lg(s);
        int have = want;
        while (have < (int)freeBy.size() && freeBy[have].empty()) ++have;
        if (have >= (int)freeBy.size()) {
            e[0] = e[1] = 0;
            ok = false;
            continue;
        }
        auto sq = freeBy[have].back();
        freeBy[have].pop_back();
        while (have > want) {  // split, keeping the top-left quarter
            --have;
            const int h = 1 << have;
            freeBy[have].push_back({sq.first + h, sq.second + h});
            freeBy[have].push_back({sq.first, sq.second + h});
            freeBy[have].push_back({sq.first + h, sq.second});
        }
        e[0] = sq.first;
        e[1] = sq.second;
    }
    packer[8] = 1;  // the original marks the packer as packed
    return ok;
}

// What the engine's repack() does after packing: copy each live region's placement.
void CopyPlacements(uint8_t* allocator, uint8_t* packer) {
    uint8_t* entries = *reinterpret_cast<uint8_t**>(packer + 0x10);
    const size_t n = *reinterpret_cast<size_t*>(packer + 0x18);
    for (int i = 0; i < 256; ++i) {
        auto r = *reinterpret_cast<uint8_t**>(allocator + 0x58 + i * 8);
        if (!r) continue;
        int handle = *reinterpret_cast<int*>(r + 0x30);
        if (handle < 0 || (size_t)handle >= n) continue;
        memcpy(r + 0x34, entries + (size_t)handle * 0x14, 16);  // x, y, w, h
    }
}

// Rendered-frame counter: repack() runs once per frame while shadows render,
// which is every gameplay frame. Used by the preset benchmark.
volatile LONG64 g_frameCount = 0;

void __fastcall RepackHook(void* self) {
    InterlockedIncrement64(&g_frameCount);
    auto a = static_cast<uint8_t*>(self);
    auto packer = *reinterpret_cast<uint8_t**>(a + 0x30);
    int W = *reinterpret_cast<int*>(packer), H = *reinterpret_cast<int*>(packer + 4);
    auto entries = *reinterpret_cast<uint8_t**>(packer + 0x10);
    size_t n = *reinterpret_cast<size_t*>(packer + 0x18);

    unsigned long long area = 0;
    int maxSide = 0;
    std::vector<int> before(n * 2);
    for (size_t i = 0; i < n; ++i) {
        int w = *reinterpret_cast<int*>(entries + i * 0x14 + 8), h = *reinterpret_cast<int*>(entries + i * 0x14 + 12);
        before[i * 2] = w;
        before[i * 2 + 1] = h;
        area += Pow2Area(w, h);
        maxSide = (std::max)(maxSide, Pow2Side(w, h));
    }

    if (g_largeSlotFix && maxSide > 2048) {
        bool ok = PackLarge(packer);
        CopyPlacements(a, packer);
        AcquireSRWLockExclusive(&g_allocLock);
        ++g_ourPacks;
        if (!ok) ++g_ourPackFailures;
        ReleaseSRWLockExclusive(&g_allocLock);
    } else {
        g_origRepack(self);
    }
    if (!g_diagRepack) return;

    // The packer may reallocate its entry array, so re-read it.
    entries = *reinterpret_cast<uint8_t**>(packer + 0x10);
    unsigned long long halved = 0, bad = 0, live = 0;
    for (size_t i = 0; i < n && i < *reinterpret_cast<size_t*>(packer + 0x18); ++i)
        if (*reinterpret_cast<int*>(entries + i * 0x14 + 8) != before[i * 2] ||
            *reinterpret_cast<int*>(entries + i * 0x14 + 12) != before[i * 2 + 1])
            ++halved;
    int bx = 0, by = 0, bw = 0, bh = 0;
    for (int i = 0; i < 256; ++i) {
        auto r = *reinterpret_cast<uint8_t**>(a + 0x58 + i * 8);
        if (!r || *reinterpret_cast<int*>(r + 0x30) < 0) continue;
        ++live;
        int x = *reinterpret_cast<int*>(r + 0x34), y = *reinterpret_cast<int*>(r + 0x38);
        int w = *reinterpret_cast<int*>(r + 0x3C), h = *reinterpret_cast<int*>(r + 0x40);
        if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > W || y + h > H) {
            ++bad;
            bx = x, by = y, bw = w, bh = h;
        }
    }

    AcquireSRWLockExclusive(&g_allocLock);
    auto& s = g_repack;
    ++s.frames;
    double fill = W > 0 && H > 0 ? (double)area / ((double)W * H) : 0;
    if (fill > 1.0) ++s.overflowFrames;
    s.maxFill = (std::max)(s.maxFill, fill);
    s.halvedEntries += halved;
    s.badPlacements += bad;
    s.maxEntries = (std::max<unsigned long long>)(s.maxEntries, n);
    s.maxLive = (std::max)(s.maxLive, live);
    if (bad) s.lastBadX = bx, s.lastBadY = by, s.lastBadW = bw, s.lastBadH = bh;
    g_allocDirty = true;
    ReleaseSRWLockExclusive(&g_allocLock);
}

bool PatchSlot(uint8_t* slot, void* hook) {
    DWORD old;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return false;
    *reinterpret_cast<void**>(slot) = hook;
    VirtualProtect(slot, 8, old, &old);
    return true;
}

// Hooks ShadowMapAllocator vtable slots: allocate() for diagnostics and light
// scaling, repack() for diagnostics and/or the >2048 slot fix.
// Returns true if allocate() diagnostics are active.
bool InstallShadowHooks(bool diag, bool fix, bool scaleLights) {
    if (!diag && !fix && !scaleLights) return false;
    // Prologue of ShadowMapAllocator::allocate up to "mov rdi, rcx".
    uint8_t* fn = FindInGame({0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74,
                              0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20,
                              0x41, 0x0F, 0xB6, 0xE9, 0x41, 0x8B, 0xF0, 0x44, 0x8B, 0xF2, 0x48, 0x8B, 0xF9});
    if (!fn) {
        Log("Shadow hooks: allocate() signature not found");
        return false;
    }
    // Find the vtable slot holding its address (data section, not executable).
    std::vector<int> ptr(8);
    for (int i = 0; i < 8; ++i) ptr[i] = (int)((reinterpret_cast<uintptr_t>(fn) >> (8 * i)) & 0xFF);
    uint8_t* slot = FindInGame(ptr, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    if (!slot) {
        Log("Shadow hooks: vtable slot for allocate() not found");
        return false;
    }

    bool hookedAllocate = false;
    if (diag || scaleLights) {
        g_origAllocate = reinterpret_cast<AllocateFn>(fn);
        g_diagAllocate = diag;
        hookedAllocate = PatchSlot(slot, reinterpret_cast<void*>(&AllocateHook));
        if (hookedAllocate)
            Log("Shadow hooks: allocate() +0x%llx hooked via vtable slot +0x%llx (diag=%d, light scaling=%d)",
                Rva(fn), Rva(slot), diag, scaleLights);
    }
    if (!fix && !diag) return false;

    // repack() is two slots further on; check its prologue before hooking.
    static const uint8_t kRepackPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57,
                                              0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF1, 0x48, 0x8B, 0x49, 0x30};
    uint8_t* slot3 = slot + 16;
    auto repack = *reinterpret_cast<uint8_t**>(slot3);
    if (Readable(repack, sizeof(kRepackPrologue)) && !memcmp(repack, kRepackPrologue, sizeof(kRepackPrologue))) {
        g_origRepack = reinterpret_cast<RepackFn>(repack);
        g_diagRepack = diag;
        g_largeSlotFix = fix;
        if (PatchSlot(slot3, reinterpret_cast<void*>(&RepackHook)))
            Log("Shadow hooks: repack() +0x%llx hooked (diag=%d, large-slot packer=%d)", Rva(repack), diag, fix);
    } else {
        Log("Shadow hooks: repack() prologue mismatch - not hooked; cascades above 2048 will flicker");
    }
    return hookedAllocate && diag;
}

// The ShadowMapAllocator ctor sets the atlas to 4096x2048 and then doubles it
// (8192x4096). Both the "ShadowMap" colour target and its depth buffer are
// created from those fields, so scaling the two immediates grows the whole atlas.
// Must run before the renderer is created, so it's called from DllMain.
//   mov dword ptr [rdi+10h], 1000h ; mov dword ptr [rdi+14h], 800h ; mov [rdi+18h], bl
// ---------------------------------------------------------------------------
// Free camera input
//
// The developer free camera (camera modes 1/2/6) still ships and is toggled by
// the TOGGLE_FREECAMERA action, which keybinds.xml can bind. But it moves via
// analog controls 9..16 (FREECAMERA_X/Z/HEADING/PITCH/UP/DOWN/FASTER/SLOWER) that
// have no PC binding, so it sat still while Alan kept walking. We detour
// float getAxis(int control) and, while the free camera is active, feed those
// controls from the player's own move/look axes (keyboard, mouse or pad) plus
// keys for up/down/faster/slower, and zero the player's controls 0..8.

using GetAxisFn = float(__fastcall*)(int, int);
GetAxisFn g_getAxisTrampoline = nullptr;
volatile int* g_cameraMode = nullptr;
volatile float g_freeCamLook = 1.0f;     // [FreeCamera] LookSensitivity
volatile LONG g_freeCamInvertPitch = 0;  // [FreeCamera] InvertPitch

bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

float KeyAxis(int vk) { return (GetAsyncKeyState(vk) & 0x8000) ? 1.0f : 0.0f; }

// Diagnostics: per-control call counts and peak |value| from the original getAxis,
// split by whether the free camera is active. Logged by the worker every 2 s.
struct AxisDiag {
    volatile LONG calls[2][256];
    volatile float peak[2][256];
};
AxisDiag g_axisDiag;
volatile LONG g_axisDiagOn = 0;

// Camera modes (setCameraMode jump table): 0 player, 1/2/6 FreeCamera, 3 vehicle?,
// 4/5 cameraman, 7 cinematic. The toggle cycles 0 -> 1 -> 2 -> 0: mode 1 is the
// flyable free camera, mode 2 freezes it in place and gives control back to Alan.
bool FreeCamControlled(int mode) { return mode == 1 || mode == 6; }

float __fastcall GetAxisHook(int id, int arg) {
    const int mode = g_cameraMode ? *g_cameraMode : 0;
    if (g_axisDiagOn && id >= 0 && id < 256) {
        const int fc = (mode == 1 || mode == 2 || mode == 6);
        InterlockedIncrement(&g_axisDiag.calls[fc][id]);
        float v = g_getAxisTrampoline(id, arg);
        if (fabsf(v) > g_axisDiag.peak[fc][id]) g_axisDiag.peak[fc][id] = fabsf(v);
    }
    if (!FreeCamControlled(mode)) return g_getAxisTrampoline(id, arg);
    const bool focus = GameHasFocus();
    switch (id) {
        case 9: return g_getAxisTrampoline(0, arg);   // FREECAMERA_X <- MOVEMENT_X
        case 10: return g_getAxisTrampoline(1, arg);  // FREECAMERA_Z <- MOVEMENT_Y
        case 11: return g_getAxisTrampoline(5, arg) * g_freeCamLook;  // HEADING <- CAMERA_X
        case 12: {                                                    // PITCH <- CAMERA_Y
            float v = g_getAxisTrampoline(6, arg) * g_freeCamLook;
            return g_freeCamInvertPitch ? -v : v;
        }
        case 13: return (std::max)(g_getAxisTrampoline(id, arg), focus ? KeyAxis(VK_SPACE) : 0.0f);     // UP
        case 14: return (std::max)(g_getAxisTrampoline(id, arg), focus ? KeyAxis(VK_LCONTROL) : 0.0f);  // DOWN
        case 15: return (std::max)(g_getAxisTrampoline(id, arg), focus ? KeyAxis(VK_LSHIFT) : 0.0f);    // FASTER
        case 16: return (std::max)(g_getAxisTrampoline(id, arg), focus ? KeyAxis(VK_LMENU) : 0.0f);     // SLOWER
        default:
            // MOVEMENT, JUMP, SHOOT, RELOAD, CAMERA: Alan stays put while you fly.
            if (id >= 0 && id <= 8) return 0.0f;
            return g_getAxisTrampoline(id, arg);
    }
}

// bool isPressed(int control): the game still polls its developer controls every
// frame, but they never fire on PC (even when bound in keybinds.xml). Our hotkeys
// queue "presses"; each query for that control consumes one and answers true, so
// the game runs its own handler on its own thread.
//   0x7F TOGGLE_FREECAMERA   0x83 PAUSE1   0x87 JUMP_ONE_FRAME (only while paused)
//   0x88 TIME_FORWARDFAST (+3600 s)   0x8A TIME_FORWARD (+60 s)   0x8B TIME_BACKWARD (-60 s)
// (TIME_BACKWARDFAST 0x89 has no handler; -1 hour queues 60 TIME_BACKWARDs.)
enum DebugAction : int {
    kToggleFreeCamera = 0x7F,
    kPause = 0x83,
    kStepFrame = 0x87,
    kTimeForwardHour = 0x88,
    kTimeForward = 0x8A,
    kTimeBackward = 0x8B,
};
using IsPressedFn = bool(__fastcall*)(int, int);
IsPressedFn g_isPressedTrampoline = nullptr;
volatile LONG g_pendingPress[256] = {};

void QueuePress(int id, int count = 1) {
    if (id >= 0 && id < 256) InterlockedExchangeAdd(&g_pendingPress[id], count);
}

bool __fastcall IsPressedHook(int id, int arg) {
    if (id >= 0 && id < 256 && g_pendingPress[id] > 0) {
        if (InterlockedDecrement(&g_pendingPress[id]) >= 0) return true;
        InterlockedIncrement(&g_pendingPress[id]);  // lost a race with another query
    }
    return g_isPressedTrampoline(id, arg);
}

// Photo mode state the game keeps in plain globals (found by signature):
//   time scale: int step (100 = 1.0x) followed by the float scale the game uses;
//   paused: byte the game's isPaused() returns.
volatile int* g_timeScaleStep = nullptr;
volatile float* g_timeScale = nullptr;
volatile uint8_t* g_paused = nullptr;
char g_photoKeys[160] = "";  // key summary for the overlay, refreshed on ini load

bool LocatePhotoState() {
    // mov eax,[step]; cmp eax,10; jle +5; sub eax,10; jmp  (TIMESCALE_DOWN handler)
    uint8_t* ts = FindInGame({0x8B, 0x05, -1, -1, -1, -1, 0x83, 0xF8, 0x0A, 0x7E, 0x05, 0x83, 0xE8, 0x0A, 0xEB});
    // cmp byte [paused],0; jne; cmp byte [stepping],0; je; xor edx,edx; mov ecx,87h (JUMP_ONE_FRAME)
    uint8_t* pz = FindInGame({0x80, 0x3D, -1, -1, -1, -1, 0x00, 0x75, -1, 0x80, 0x3D, -1, -1, -1, -1, 0x00, 0x74,
                              -1, 0x33, 0xD2, 0xB9, 0x87, 0x00, 0x00, 0x00});
    if (ts) {
        g_timeScaleStep = reinterpret_cast<volatile int*>(const_cast<uint8_t*>(RipTarget(ts, 2, 6)));
        g_timeScale = reinterpret_cast<volatile float*>(g_timeScaleStep + 1);
    }
    if (pz) g_paused = const_cast<volatile uint8_t*>(RipTarget(pz, 2, 7));
    Log("Photo mode: time scale %s, paused flag %s", ts ? "found" : "NOT found", pz ? "found" : "NOT found");
    return ts && pz;
}

// Sets game speed directly (the game's own keys only step by 0.1). 1.0 = normal.
void SetTimeScale(float s) {
    if (!g_timeScaleStep) return;
    s = (std::max)(0.01f, (std::min)(4.0f, s));
    *g_timeScaleStep = (int)lroundf(s * 100.0f);
    *g_timeScale = *g_timeScaleStep / 100.0f;
}

// Writes a 14-byte "jmp [rip+0]; dq target" at `at`, padding `len - 14` with nops.
bool WriteAbsJump(uint8_t* at, size_t len, const void* target) {
    uint8_t patch[32] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(patch + 6, &target, 8);
    for (size_t i = 14; i < len; ++i) patch[i] = 0x90;
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, patch, len);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return true;
}

// isPressed starts with  cmp qword ptr [rip+X],0 (8) / je early_out (6): exactly
// 14 bytes. The trampoline redoes the compare through rax and both branches.
bool InstallTogglePressHook() {
    uint8_t* fn = FindInGame({0x48, 0x83, 0x3D, -1, -1, -1, -1, 0x00, 0x0F, 0x84, -1, -1, -1, -1, 0x48, 0x63,
                              0x15, -1, -1, -1, -1, 0x48, 0x8B, 0x05, -1, -1, -1, -1, 0x4C, 0x8B, 0x0C, 0xD0});
    if (!fn) {
        Log("Free camera: isPressed signature not found - toggle key unavailable");
        return false;
    }
    const uint8_t* cmpTarget = RipTarget(fn, 3, 8);
    const uint8_t* earlyOut = RipTarget(fn + 8, 2, 6);
    auto t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    uint8_t* p = t;
    auto put = [&](std::initializer_list<uint8_t> b) {
        for (uint8_t x : b) *p++ = x;
    };
    auto put64 = [&](const void* v) {
        memcpy(p, &v, 8);
        p += 8;
    };
    put({0x48, 0xB8});              // mov rax, imm64
    put64(cmpTarget);
    put({0x48, 0x83, 0x38, 0x00});  // cmp qword ptr [rax], 0
    put({0x75, 0x0E});              // jne +14 (skip the early-out jump)
    put({0xFF, 0x25, 0, 0, 0, 0});  // jmp [rip] -> early out (returns false)
    put64(earlyOut);
    put({0xFF, 0x25, 0, 0, 0, 0});  // jmp [rip] -> rest of isPressed
    put64(fn + 14);
    FlushInstructionCache(GetCurrentProcess(), t, p - t);
    g_isPressedTrampoline = reinterpret_cast<IsPressedFn>(t);
    if (!WriteAbsJump(fn, 14, reinterpret_cast<void*>(&IsPressedHook))) return false;
    Log("Free camera: isPressed +0x%llx detoured (toggle key)", Rva(fn));
    return true;
}

// Detours getAxis with a 14-byte absolute jump. The overwritten prologue is
//   sub rsp,38h / cmp qword ptr [rip+X],0 / movsxd r8,ecx   (15 bytes)
// and the trampoline rebuilds the RIP-relative cmp through rax (volatile, free
// at function entry); jmp preserves the flags for the following je.
bool InstallFreeCameraHook() {
    uint8_t* fn = FindInGame({0x48, 0x83, 0xEC, 0x38, 0x48, 0x83, 0x3D, -1, -1, -1, -1, 0x00, 0x4C, 0x63, 0xC1,
                              0x0F, 0x84});
    // movsxd rax,[mode]; cmp eax,1; jne; call; neg rax; sbb esi,esi  (free-camera toggle)
    uint8_t* modeRef = FindInGame({0x48, 0x63, 0x05, -1, -1, -1, -1, 0x83, 0xF8, 0x01, 0x75, -1, 0xE8, -1, -1, -1,
                                   -1, 0x48, 0xF7, 0xD8, 0x1B, 0xF6});
    if (!fn || !modeRef) {
        Log("Free camera: signature not found (getAxis=%d, mode=%d) - not installed", fn != nullptr,
            modeRef != nullptr);
        return false;
    }
    g_cameraMode = reinterpret_cast<volatile int*>(const_cast<uint8_t*>(RipTarget(modeRef, 3, 7)));
    const uint8_t* cmpTarget = RipTarget(fn + 4, 3, 8);

    auto t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    uint8_t* p = t;
    auto put = [&](std::initializer_list<uint8_t> b) {
        for (uint8_t x : b) *p++ = x;
    };
    auto put64 = [&](const void* v) {
        memcpy(p, &v, 8);
        p += 8;
    };
    put({0x48, 0x83, 0xEC, 0x38});  // sub rsp, 38h
    put({0x48, 0xB8});              // mov rax, imm64
    put64(cmpTarget);
    put({0x48, 0x83, 0x38, 0x00});  // cmp qword ptr [rax], 0
    put({0x4C, 0x63, 0xC1});        // movsxd r8, ecx
    put({0xFF, 0x25, 0, 0, 0, 0});  // jmp [rip+0]
    put64(fn + 15);
    FlushInstructionCache(GetCurrentProcess(), t, p - t);
    g_getAxisTrampoline = reinterpret_cast<GetAxisFn>(t);

    uint8_t patch[15] = {0xFF, 0x25, 0, 0, 0, 0};
    void* hook = reinterpret_cast<void*>(&GetAxisHook);
    memcpy(patch + 6, &hook, 8);
    patch[14] = 0x90;  // nop: remainder of the movsxd
    DWORD old;
    if (!VirtualProtect(fn, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(fn, patch, sizeof(patch));
    VirtualProtect(fn, sizeof(patch), old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof(patch));
    Log("Free camera: getAxis +0x%llx detoured, camera mode at +0x%llx", Rva(fn), Rva((void*)g_cameraMode));
    return true;
}

void LogAxisDiag() {
    for (int fc = 0; fc < 2; ++fc) {
        std::string line;
        for (int id = 0; id < 256; ++id) {
            LONG n = InterlockedExchange(&g_axisDiag.calls[fc][id], 0);
            float pk = g_axisDiag.peak[fc][id];
            g_axisDiag.peak[fc][id] = 0;
            if (!n) continue;
            char buf[48];
            snprintf(buf, sizeof(buf), " %d:%ld/%.2f", id, n, pk);
            line += buf;
        }
        if (!line.empty()) Log("getAxis %s (id:calls/peak):%s", fc ? "FREECAM" : "normal ", line.c_str());
    }
}

int g_atlasScale = 1;  // set from DllMain once the patch succeeds

bool PatchAtlasSize(int scale) {
    if (scale <= 1) return false;
    uint8_t* hit = FindInGame({0xC7, 0x47, 0x10, 0x00, 0x10, 0x00, 0x00, 0xC7, 0x47, 0x14, 0x00, 0x08,
                               0x00, 0x00, 0x88, 0x5F, 0x18});
    if (!hit) {
        Log("Atlas patch: signature not found - left at default");
        return false;
    }
    const int32_t w = 0x1000 * scale, h = 0x800 * scale;
    DWORD old;
    if (!VirtualProtect(hit, 14, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(hit + 3, &w, 4);
    memcpy(hit + 10, &h, 4);
    VirtualProtect(hit, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hit, 14);
    Log("Atlas patch: shadow atlas %dx%d -> %dx%d (+0x%llx)", 0x2000, 0x1000, w * 2, h * 2, Rva(hit));
    return true;
}

void LogAllocStats() {
    AcquireSRWLockExclusive(&g_allocLock);
    if (!g_allocDirty) {
        ReleaseSRWLockExclusive(&g_allocLock);
        return;
    }
    g_allocDirty = false;
    // Stats are per interval, so the effect of a live ini change shows up.
    auto stats = std::move(g_allocStats);
    g_allocStats.clear();
    int aw = g_atlasW, ah = g_atlasH, large = g_atlasLarge;
    RepackStats rp = g_repack;
    g_repack = RepackStats{};
    unsigned long long ourPacks = g_ourPacks, ourFails = g_ourPackFailures;
    ReleaseSRWLockExclusive(&g_allocLock);

    Log("Shadow atlas %dx%d large=%d, last interval:", aw, ah, large);
    struct Agg {
        unsigned long long calls = 0, failures = 0;
        int minW = INT_MAX, maxW = 0, minH = INT_MAX, maxH = 0;
    };
    std::map<std::pair<unsigned long long, int>, Agg> byCaller;
    for (auto& [k, s] : stats) {
        auto& a = byCaller[{k.caller, k.flag}];
        a.calls += s.calls;
        a.failures += s.failures;
        a.minW = (std::min)(a.minW, k.w), a.maxW = (std::max)(a.maxW, k.w);
        a.minH = (std::min)(a.minH, k.h), a.maxH = (std::max)(a.maxH, k.h);
    }
    for (auto& [k, a] : byCaller)
        Log("  allocate caller=+0x%llx flag=%d calls=%llu failed=%llu w=%d..%d h=%d..%d", k.first, k.second,
            a.calls, a.failures, a.minW, a.maxW, a.minH, a.maxH);
    if (rp.frames)
        Log("  repack frames=%llu maxEntries=%llu maxLive=%llu maxFill=%.2f overflowFrames=%llu "
            "halvedEntries=%llu badPlacements=%llu | large-slot packer total=%llu failed=%llu",
            rp.frames, rp.maxEntries, rp.maxLive, rp.maxFill, rp.overflowFrames, rp.halvedEntries,
            rp.badPlacements, ourPacks, ourFails);
    if (rp.badPlacements)
        Log("  last bad region: x=%d y=%d w=%d h=%d", rp.lastBadX, rp.lastBadY, rp.lastBadW, rp.lastBadH);
}

const char* TweakName(uint8_t* obj) {
    if (!Readable(obj, kObjSize)) return nullptr;
    const char* name = *reinterpret_cast<const char**>(obj + kOffName);
    if (!Readable(name, 1)) return nullptr;
    for (int i = 0; i < 256; ++i) {
        if (!Readable(name + i, 1)) return nullptr;
        unsigned char c = name[i];
        if (c == 0) return i ? name : nullptr;
        if (c < 0x20 || c > 0x7E) return nullptr;
    }
    return nullptr;
}

int TweakType(uint8_t* obj) { return *reinterpret_cast<int*>(obj + kOffType); }

// ---------------------------------------------------------------------------
// Config

struct Override {
    std::string raw;
    float f[4] = {0, 0, 0, 0};
    int count = 0;  // number of parsed floats
    bool isBool = false;
    bool b = false;
    bool noClamp = false;  // never clamp to the engine range (packed shader settings)
};

struct Config {
    bool enabled = true;
    bool enforce = true;
    bool clampToRange = false;
    bool dumpAll = true;
    int pollMs = 100;
    bool logShadowAllocs = false;  // [Debug]; read at startup only
    bool logFreeCamInput = false;  // [Debug]; read at startup only
    int benchSettleMs = 2000, benchMeasureMs = 5000;  // [Debug] BenchmarkSettleMs / BenchmarkMeasureMs
    int lightShadowScale = 1;      // [Shadows]; live
    float freeCamLook = 1.0f;      // [FreeCamera] LookSensitivity; live
    int freeCamKey = VK_F7;        // [FreeCamera] ToggleKey; live
    // [PhotoMode] keys; live
    int pauseKey = VK_F8, stepKey = VK_F6;
    int slowerKey = VK_OEM_MINUS, fasterKey = VK_OEM_PLUS, normalSpeedKey = VK_BACK;
    int timeForwardKey = VK_PRIOR, timeBackwardKey = VK_NEXT;  // PageUp / PageDown
    bool freeCamInvertPitch = false;
    int toggleKey = VK_F10;        // A/B: all overrides off/on
    int presetKey = VK_F9;         // cycle presets
    bool beep = true;              // audible feedback for the hotkeys
    // [Settings] Preset=<name>[, <name>...]: active at start, applied on top of [Tweaks].
    std::vector<std::string> startPresets;
    std::string presetLine;        // raw Preset= value, to notice when the ini changes it
    std::unordered_map<std::string, Override> tweaks;
    // [Preset:<name>] sections, in file order. Names keep their case for display.
    struct PresetDef {
        std::string name;
        std::string group;        // Group=: at most one active preset per group ("" = no limit)
        std::string description;  // Description=: shown in the overlay
        std::unordered_map<std::string, Override> tweaks;
    };
    std::vector<PresetDef> presets;
};

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool ParseBool(const std::string& v, bool* out) {
    std::string l = Lower(v);
    if (l == "1" || l == "true" || l == "on" || l == "yes") return *out = true, true;
    if (l == "0" || l == "false" || l == "off" || l == "no") return *out = false, true;
    return false;
}

bool ParseOverride(const std::string& v, Override* o) {
    o->raw = v;
    o->isBool = ParseBool(v, &o->b);
    const char* p = v.c_str();
    while (*p && o->count < 4) {
        char* end;
        errno = 0;
        float f = strtof(p, &end);
        if (end == p || errno) break;
        o->f[o->count++] = f;
        p = end;
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
    }
    // Anything else is kept as text: list tweakables take an option name.
    return !v.empty();
}

// Named keys accepted in the ini (case-insensitive) and shown back to the user.
const std::pair<const char*, int> kNamedKeys[] = {
    {"PageUp", VK_PRIOR},  {"PageDown", VK_NEXT},     {"Home", VK_HOME},          {"End", VK_END},
    {"Insert", VK_INSERT}, {"Delete", VK_DELETE},     {"Backspace", VK_BACK},     {"Space", VK_SPACE},
    {"Minus", VK_OEM_MINUS}, {"Plus", VK_OEM_PLUS},   {"NumpadMinus", VK_SUBTRACT}, {"NumpadPlus", VK_ADD},
    {"Pause", VK_PAUSE},   {"ScrollLock", VK_SCROLL}};

// "F1".."F24", a single letter or digit, a named key, "0x.." virtual-key code, or "None".
int ParseKey(const std::string& v) {
    std::string l = Lower(v);
    if (l == "none" || l == "0" || l == "off") return 0;
    for (auto& [n, vk] : kNamedKeys)
        if (l == Lower(n)) return vk;
    if (l.size() >= 2 && l[0] == 'f' && isdigit((unsigned char)l[1])) {
        int n = atoi(l.c_str() + 1);
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : -1;
    }
    if (l.size() == 1 && isalnum((unsigned char)l[0])) return toupper((unsigned char)l[0]);
    if (l.rfind("0x", 0) == 0) return (int)strtol(l.c_str() + 2, nullptr, 16);
    return -1;
}

std::string KeyName(int vk) {
    if (!vk) return "none";
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    for (auto& [n, code] : kNamedKeys)
        if (code == vk) return n;
    // Virtual-key codes equal ASCII only for 0-9 and A-Z (0x60+ are numpad keys).
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

Config LoadConfig(const std::wstring& path) {
    Config cfg;
    Config::PresetDef* presetDef = nullptr;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        Log("No config at awtweaks.ini - nothing to apply");
        return cfg;
    }
    char line[1024];
    std::string section;
    int lineNo = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineNo;
        std::string s = Trim(line);
        if (lineNo == 1 && s.size() >= 3 && (uint8_t)s[0] == 0xEF) s = Trim(s.substr(3));  // BOM
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') {
            std::string raw = Trim(s.substr(1, s.find(']') - 1));
            section = Lower(raw);
            presetDef = nullptr;
            if (section.rfind("preset:", 0) == 0) {
                cfg.presets.push_back({});
                presetDef = &cfg.presets.back();
                presetDef->name = Trim(raw.substr(7));
                section = "preset";
            }
            continue;
        }
        // Keys contain ':' and spaces, so split on the last '=' (values never contain '=').
        size_t eq = s.rfind('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(s.substr(0, eq));
        std::string val = Trim(s.substr(eq + 1));
        size_t c = val.find_first_of(";#");  // trailing comment
        if (c != std::string::npos) val = Trim(val.substr(0, c));
        if (key.empty() || val.empty()) continue;

        if (section == "settings") {
            std::string k = Lower(key);
            bool b;
            if (k == "enabled" && ParseBool(val, &b)) cfg.enabled = b;
            else if (k == "enforce" && ParseBool(val, &b)) cfg.enforce = b;
            else if (k == "clamptorange" && ParseBool(val, &b)) cfg.clampToRange = b;
            else if (k == "dumpall" && ParseBool(val, &b)) cfg.dumpAll = b;
            else if (k == "pollms") cfg.pollMs = (std::max)(16, (std::min)(5000, atoi(val.c_str())));
            else if (k == "beep" && ParseBool(val, &b)) cfg.beep = b;
            else if (k == "preset") {
                cfg.presetLine = val;
                size_t p = 0;
                while (p <= val.size()) {
                    size_t c = val.find(',', p);
                    std::string one = Trim(val.substr(p, c == std::string::npos ? std::string::npos : c - p));
                    if (!one.empty() && Lower(one) != "none") cfg.startPresets.push_back(one);
                    if (c == std::string::npos) break;
                    p = c + 1;
                }
            }
            else if (k == "togglekey" || k == "presetkey") {
                int vk = ParseKey(val);
                if (vk < 0) Log("ini:%d can't parse key '%s' (use F1-F24, a letter/digit, 0x.., or None)", lineNo, val.c_str());
                else (k == "togglekey" ? cfg.toggleKey : cfg.presetKey) = vk;
            }
            else Log("ini:%d unknown setting '%s'", lineNo, key.c_str());
        } else if (section == "shadows") {
            std::string k = Lower(key);
            if (k == "lightshadowscale") {
                int v = atoi(val.c_str());
                cfg.lightShadowScale = v == 2 || v == 4 ? v : 1;
                if (v != cfg.lightShadowScale) Log("ini:%d LightShadowScale must be 1, 2 or 4; using 1", lineNo);
            } else if (k != "atlasscale") {  // AtlasScale is read in DllMain
                Log("ini:%d unknown shadows setting '%s'", lineNo, key.c_str());
            }
        } else if (section == "freecamera") {
            std::string k = Lower(key);
            bool b;
            if (k == "looksensitivity") cfg.freeCamLook = (float)atof(val.c_str());
            else if (k == "invertpitch" && ParseBool(val, &b)) cfg.freeCamInvertPitch = b;
            else if (k == "togglekey") {
                int vk = ParseKey(val);
                if (vk < 0) Log("ini:%d can't parse key '%s'", lineNo, val.c_str());
                else cfg.freeCamKey = vk;
            }
            else if (k != "enabled") Log("ini:%d unknown free camera setting '%s'", lineNo, key.c_str());
        } else if (section == "photomode") {
            std::string k = Lower(key);
            int* slot = k == "pausekey" ? &cfg.pauseKey
                        : k == "stepkey" ? &cfg.stepKey
                        : k == "slowerkey" ? &cfg.slowerKey
                        : k == "fasterkey" ? &cfg.fasterKey
                        : k == "normalspeedkey" ? &cfg.normalSpeedKey
                        : k == "timeforwardkey" ? &cfg.timeForwardKey
                        : k == "timebackwardkey" ? &cfg.timeBackwardKey
                        : nullptr;
            if (slot) {
                int vk = ParseKey(val);
                if (vk < 0) Log("ini:%d can't parse key '%s'", lineNo, val.c_str());
                else *slot = vk;
            } else if (k != "enabled") {
                Log("ini:%d unknown photo mode setting '%s'", lineNo, key.c_str());
            }
        } else if (section == "debug") {
            bool b;
            if (Lower(key) == "logshadowallocs" && ParseBool(val, &b)) cfg.logShadowAllocs = b;
            else if (Lower(key) == "logfreecaminput" && ParseBool(val, &b)) cfg.logFreeCamInput = b;
            else if (Lower(key) == "benchmarksettlems") cfg.benchSettleMs = (std::max)(0, atoi(val.c_str()));
            else if (Lower(key) == "benchmarkmeasurems") cfg.benchMeasureMs = (std::max)(50, atoi(val.c_str()));
            else Log("ini:%d unknown debug setting '%s'", lineNo, key.c_str());
        } else if (section == "preset" && (Lower(key) == "group" || Lower(key) == "description")) {
            // Metadata, not a tweakable (tweakable names always contain "Category:").
            // Descriptions may contain ';', so take the raw text after '='.
            if (Lower(key) == "group") presetDef->group = val;
            else presetDef->description = Trim(s.substr(s.find('=') + 1));
        } else if (section == "tweaks" || section == "preset") {
            Override o;
            if (ParseOverride(val, &o)) (presetDef ? presetDef->tweaks : cfg.tweaks)[key] = o;
            else Log("ini:%d could not parse value '%s' for '%s'", lineNo, val.c_str(), key.c_str());
        }
    }
    fclose(f);
    Log("Config loaded: enabled=%d enforce=%d clamp=%d poll=%dms, %zu overrides, %zu presets, "
        "toggle=%s preset key=%s",
        cfg.enabled, cfg.enforce, cfg.clampToRange, cfg.pollMs, cfg.tweaks.size(), cfg.presets.size(),
        KeyName(cfg.toggleKey).c_str(), KeyName(cfg.presetKey).c_str());
    return cfg;
}

// Index of the preset called `name` (case-insensitive), or -1.
int FindPreset(const Config& base, const std::string& name) {
    for (size_t i = 0; i < base.presets.size(); ++i)
        if (Lower(base.presets[i].name) == Lower(name)) return (int)i;
    return -1;
}

// Where an applied value comes from (reported to the overlay).
enum Source : int { kSrcNone = 0, kSrcTweaks = 1, kSrcPreset = 2, kSrcOverlay = 3 };

// Shared between the worker, the hotkeys and the overlay API; guarded by g_knownLock.
//  - g_ui: overlay edits. They win over [Tweaks] and presets until saved or undone.
//  - g_activePresets: active preset names, at most one per Group, kept in file order.
//  - g_modOff: the A/B toggle.
// Any change bumps g_uiVersion so the worker re-derives what to apply.
std::unordered_map<std::string, Override> g_ui;
std::unordered_map<std::string, int> g_sourceOf;            // name -> Source for the current cfg
std::unordered_map<std::string, int> g_iniSourceOf;         // same, ignoring overlay edits
std::unordered_map<std::string, std::string> g_presetOf;    // name -> preset that sets it (winning one)
std::vector<std::string> g_activePresets;
float g_sunSize = 0.0f, g_contactShadows = 0.0f;  // effective shadow-shader settings
int g_shadowFxSource = kSrcNone;
Config g_baseShared;  // last loaded ini, for preset metadata (names/groups/descriptions)
bool g_modOff = false;
volatile LONG g_uiVersion = 0;
SRWLOCK g_knownLock = SRWLOCK_INIT;

// Activate/deactivate a preset by name, enforcing one per group. Lock held.
bool SetPresetActiveLocked(const std::string& name, bool on) {
    int idx = FindPreset(g_baseShared, name);
    if (idx < 0) return false;
    const auto& def = g_baseShared.presets[idx];
    auto& act = g_activePresets;
    act.erase(std::remove_if(act.begin(), act.end(),
                             [&](const std::string& n) {
                                 if (Lower(n) == Lower(def.name)) return true;
                                 int j = FindPreset(g_baseShared, n);
                                 return on && !def.group.empty() && j >= 0 &&
                                        Lower(g_baseShared.presets[j].group) == Lower(def.group);
                             }),
              act.end());
    if (on) act.push_back(def.name);
    // Keep file order so overlapping presets apply deterministically.
    std::sort(act.begin(), act.end(), [](const std::string& a, const std::string& b) {
        return FindPreset(g_baseShared, a) < FindPreset(g_baseShared, b);
    });
    return true;
}

// Preset benchmark: Stock and each Quality preset in turn, 2 s to settle then
// 5 s of frame counting each; the user's presets are restored afterwards.
// Guarded by g_knownLock.
struct Benchmark {
    bool running = false;
    bool requested = false;
    std::vector<std::string> stages;  // "" = Stock (no Quality preset)
    int stage = -1;
    ULONGLONG stageStart = 0;
    LONG64 framesAtMeasure = 0;
    bool measuring = false;
    std::vector<std::string> savedPresets;
    std::vector<std::pair<std::string, float>> results;  // name ("Stock" or preset), average fps
} g_bench;

std::string JoinActivePresetsLocked() {
    std::string s;
    for (auto& n : g_activePresets) s += (s.empty() ? "" : ", ") + n;
    return s;
}

// Sun-cascade resolutions above 2048 need a power-of-two slot the game's packer
// can't place; with a stock-size atlas it then loops forever (the game freezes).
// Only allow them when our large-slot packer is installed (AtlasScale=2), and
// never past 4096 (six 8192 slots can't fit even the enlarged atlas).
bool IsCascadeResolution(const std::string& name) {
    return name == "Shadows:High Resolution" || name == "Shadows:Very High Resolution" ||
           name == "Shadows:Ultra High Resolution";
}

float SafeCascadeResolution(const std::string& name, float want) {
    const float limit = g_largeSlotFix ? 4096.0f : 2048.0f;
    if (!IsCascadeResolution(name) || want <= limit) return want;
    static std::unordered_set<std::string> warned;
    if (warned.insert(name + (g_largeSlotFix ? "/4096" : "/2048")).second)
        Log("WARNING: %s=%g capped to %g (%s)", name.c_str(), want, limit,
            g_largeSlotFix ? "6 cascades above 4096 can't fit the 16384x8192 atlas"
                           : "needs [Shadows] AtlasScale=2 - the game's packer hangs otherwise");
    return limit;
}

// Settings for AWTweaks' shadow shader (the ReShade add-on's [Shaders] ShadowShader=
// Enhanced). They look like tweakables, so [Tweaks], presets and the overlay all handle
// them, but they reach the GPU packed into the low digits of "Cascade Filter Size": the
// engine only uploads constants the game's shader reads, and only that shader reads it.
const char* const kSunSizeName = "AWTweaks:Sun Size";              // degrees, 0 = stock filter
const char* const kContactShadowsName = "AWTweaks:Contact Shadows";  // metres, 0 = off
const char* const kPackedName = "Shadow Filtering d3t:Cascade Filter Size";
float g_stockCascadeFilterSize = 9.0f;  // the engine's value, noted when it registers

bool IsShadowFxName(const std::string& name) {
    return Lower(name) == Lower(kSunSizeName) || Lower(name) == Lower(kContactShadowsName);
}

// Filter size rounded to 0.1, plus 0.00001 * (1 + sun size in 0.1 degree steps * 100
// + contact length in 2 cm steps); see src/shaders/shadow_buffer.hlsl DecodeSettings.
float PackShadowFx(float filterSize, float sunDegrees, float contactMetres) {
    int sun = (int)std::lround(std::clamp(sunDegrees, 0.0f, 4.9f) * 10.0f);
    int contact = (int)std::lround(std::clamp(contactMetres, 0.0f, 1.98f) / 0.02f);
    double size = std::floor(std::clamp(filterSize, 1.0f, 16.0f) * 10.0 + 0.5) / 10.0;
    return (float)(size + (1 + sun * 100 + contact) * 0.00001);
}

// The shader reads settings from the digits below 0.1, so a size someone sets is kept
// on the 0.1 grid (9.25 would otherwise read as settings).
float RoundFilterSize(float v) { return (float)(std::floor(v * 10.0 + 0.5) / 10.0); }

// Moves the two settings out of cfg (the engine has no such tweakables) into the
// "Cascade Filter Size" override, on top of whatever size is set or stock. Lock held.
void DeriveShadowFx(Config& cfg) {
    g_sunSize = g_contactShadows = 0.0f;
    g_shadowFxSource = kSrcNone;
    auto sized = cfg.tweaks.find(kPackedName);
    if (sized != cfg.tweaks.end() && sized->second.count) sized->second.f[0] = RoundFilterSize(sized->second.f[0]);
    for (auto it = cfg.tweaks.begin(); it != cfg.tweaks.end();) {
        if (!IsShadowFxName(it->first)) {
            ++it;
            continue;
        }
        float v = it->second.count ? (std::max)(0.0f, it->second.f[0]) : 0.0f;
        (Lower(it->first) == Lower(kSunSizeName) ? g_sunSize : g_contactShadows) = v;
        g_shadowFxSource = (std::max)(g_shadowFxSource, g_sourceOf[it->first]);
        it = cfg.tweaks.erase(it);
    }
    if (g_sunSize <= 0.0f && g_contactShadows <= 0.0f) return;
    float size = g_stockCascadeFilterSize;
    auto set = cfg.tweaks.find(kPackedName);
    if (set != cfg.tweaks.end() && set->second.count) size = set->second.f[0];
    Override o;
    o.f[0] = PackShadowFx(size, g_sunSize, g_contactShadows);
    o.count = 1;
    o.noClamp = true;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.5f", o.f[0]);
    o.raw = buf;
    cfg.tweaks[kPackedName] = o;
    if (g_sourceOf[kPackedName] < g_shadowFxSource) g_sourceOf[kPackedName] = g_shadowFxSource;
}

// What actually gets applied: [Tweaks], the active presets on top (file order),
// then overlay edits; nothing at all while the mod is toggled off. Lock held.
Config Derive(const Config& base) {
    Config cfg = base;
    g_sourceOf.clear();
    g_presetOf.clear();
    for (auto& [name, o] : base.tweaks) g_sourceOf[name] = kSrcTweaks;
    for (auto& def : base.presets) {
        bool active = false;
        for (auto& n : g_activePresets) active |= Lower(n) == Lower(def.name);
        if (!active) continue;
        for (auto& [name, o] : def.tweaks) {
            cfg.tweaks[name] = o;
            g_sourceOf[name] = kSrcPreset;
            g_presetOf[name] = def.name;
        }
    }
    g_iniSourceOf = g_sourceOf;
    for (auto& [name, o] : g_ui) {
        cfg.tweaks[name] = o;
        g_sourceOf[name] = kSrcOverlay;
    }
    DeriveShadowFx(cfg);
    for (auto& [name, o] : cfg.tweaks)
        if (IsCascadeResolution(name) && o.count) o.f[0] = SafeCascadeResolution(name, o.f[0]);
    if (g_modOff) cfg.enabled = false;
    return cfg;
}

// Edge-detects a key press while the game window has focus.
bool KeyPressed(int vk, bool* wasDown) {
    if (!vk) return false;
    SHORT s = GetAsyncKeyState(vk);
    bool down = (s & 0x8000) != 0;
    // Bit 0 = "pressed since the last call": catches taps shorter than PollMs.
    bool pressed = !*wasDown && (down || (s & 1));
    *wasDown = down;
    if (!pressed) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

FILETIME FileTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return FILETIME{};
    return d.ftLastWriteTime;
}

// ---------------------------------------------------------------------------
// Apply

struct Known {
    uint8_t* obj;
    std::string name;
    int type;
    uint8_t original[16];  // value as the engine registered it, before any override
    bool touched = false;  // we have written to it
};

// Every registered tweakable. A deque so element addresses stay put as it grows.
// Written by the worker and the overlay API under g_knownLock.
std::deque<Known> g_known;
std::unordered_map<std::string, size_t> g_knownIndex;

void Restore(Known& k) {
    if (k.type == kList) memcpy(k.obj + kOffListIndex, k.original, 4);
    else memcpy(k.obj + kOffValue, k.original, k.type == kBool ? 1 : k.type == kFloat ? 4 : 16);
    k.touched = false;
    Log("  %s restored to original", k.name.c_str());
}

// Returns true if memory was changed.
bool Apply(const Known& k, const Override& o, const Config& cfg, bool verbose) {
    uint8_t* v = k.obj + kOffValue;
    switch (k.type) {
        case kBool: {
            bool want;
            if (o.isBool) want = o.b;
            else if (o.count) want = o.f[0] != 0.0f;
            else return false;
            uint8_t nv = want ? 1 : 0;
            if (*v == nv) return false;
            if (verbose) Log("  %s: %d -> %d", k.name.c_str(), *v, nv);
            *v = nv;
            return true;
        }
        case kFloat: {
            if (!o.count) {
                if (verbose) Log("  %s is a float; value '%s' is not a number", k.name.c_str(), o.raw.c_str());
                return false;
            }
            float* f = reinterpret_cast<float*>(v);
            float want = o.f[0];
            float lo = f[1], hi = f[2];
            if (lo <= hi && (want < lo || want > hi) && !o.noClamp) {
                if (cfg.clampToRange) want = want < lo ? lo : hi;
                if (verbose)
                    Log("  note: %s=%g is outside engine range [%g, %g]%s", k.name.c_str(), o.f[0], lo,
                        hi, cfg.clampToRange ? ", clamped" : "");
            }
            if (f[0] == want) return false;
            if (verbose) Log("  %s: %g -> %g", k.name.c_str(), f[0], want);
            f[0] = want;
            return true;
        }
        case kColor: {
            if (o.count < 3) {
                if (verbose) Log("  %s is a color; give 3 or 4 numbers (r,g,b[,a])", k.name.c_str());
                return false;
            }
            float* f = reinterpret_cast<float*>(v);
            float want[4] = {o.f[0], o.f[1], o.f[2], o.count >= 4 ? o.f[3] : f[3]};
            if (!memcmp(f, want, sizeof(want))) return false;
            if (verbose)
                Log("  %s: (%g,%g,%g,%g) -> (%g,%g,%g,%g)", k.name.c_str(), f[0], f[1], f[2], f[3],
                    want[0], want[1], want[2], want[3]);
            memcpy(f, want, sizeof(want));
            return true;
        }
        case kList: {
            // An option name (case-insensitive) or its 0-based index.
            const int n = ListCount(k.obj);
            int want = -1;
            for (int i = 0; i < n && want < 0; ++i)
                if (const char* item = ListItem(k.obj, i))
                    if (Lower(item) == Lower(o.raw)) want = i;
            if (want < 0 && o.count) want = (int)o.f[0];
            if (want < 0 || want >= n) {
                // A typo shouldn't leave an older choice in place: fall back to stock.
                memcpy(&want, k.original, 4);
                if (verbose) {
                    std::string opts;
                    for (int i = 0; i < n; ++i)
                        if (const char* item = ListItem(k.obj, i)) opts += (i ? " | " : "") + std::string(item);
                    const char* stock = ListItem(k.obj, want);
                    Log("  %s: '%s' is not an option (%s); using stock %s", k.name.c_str(), o.raw.c_str(),
                        opts.c_str(), stock ? stock : "?");
                }
                int& cur = ListIndex(k.obj);
                if (cur == want) return false;
                cur = want;
                return true;
            }
            int& cur = ListIndex(k.obj);
            if (cur == want) return false;
            if (verbose) {
                const char* from = ListItem(k.obj, cur);
                const char* to = ListItem(k.obj, want);
                Log("  %s: %s -> %s", k.name.c_str(), from ? from : "?", to ? to : "?");
            }
            cur = want;
            return true;
        }
        default:
            if (verbose) Log("  %s has unsupported type %d; skipped", k.name.c_str(), k.type);
            return false;
    }
}

void Dump(const std::deque<Known>& known, const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fprintf(f, "; AWTweaks dump of all registered engine tweakables (%zu).\n", known.size());
    fprintf(f, "; Copy any line into [Tweaks] in awtweaks.ini to override it.\n");
    fprintf(f, "; Tweakables register lazily, so this file grows as you play.\n\n");
    std::vector<const Known*> sorted;
    for (auto& k : known) sorted.push_back(&k);
    std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->name < b->name; });
    // Values shown are the engine's originals; "now" marks ones currently overridden.
    for (auto* k : sorted) {
        const uint8_t* v = k->original;
        const float* fl = reinterpret_cast<const float*>(v);
        const float* cur = reinterpret_cast<const float*>(k->obj + kOffValue);
        switch (k->type) {
            case kBool:
                fprintf(f, "%s=%s%s\n", k->name.c_str(), *v ? "true" : "false",
                        k->touched ? (k->obj[kOffValue] ? "    ; now true" : "    ; now false") : "");
                break;
            case kFloat:
                fprintf(f, "%s=%g    ; float, range %g..%g", k->name.c_str(), fl[0], cur[1], cur[2]);
                if (k->touched) fprintf(f, ", now %g", cur[0]);
                fputc('\n', f);
                break;
            case kColor:
                fprintf(f, "%s=%g, %g, %g, %g    ; color", k->name.c_str(), fl[0], fl[1], fl[2], fl[3]);
                if (k->touched) fprintf(f, ", now %g, %g, %g, %g", cur[0], cur[1], cur[2], cur[3]);
                fputc('\n', f);
                break;
            case kList: {
                int orig;
                memcpy(&orig, k->original, 4);
                const char* item = ListItem(k->obj, orig);
                fprintf(f, "%s=%s    ; list:", k->name.c_str(), item ? item : "?");
                for (int i = 0; i < ListCount(k->obj); ++i)
                    if (const char* it = ListItem(k->obj, i)) fprintf(f, "%s %s", i ? " |" : "", it);
                if (k->touched)
                    if (const char* now = ListItem(k->obj, ListIndex(k->obj))) fprintf(f, ", now %s", now);
                fputc('\n', f);
                break;
            }
            default: fprintf(f, ";%s    ; type %d (not editable)\n", k->name.c_str(), k->type); break;
        }
    }
    fclose(f);
}

DWORD WINAPI Worker(LPVOID) {
    const std::wstring iniPath = g_dir + L"awtweaks.ini";
    const std::wstring dumpPath = g_dir + L"awtweaks_dump.txt";

    // This thread starts once the loader releases its lock, before the game's own
    // init runs, so hooks installed here see the renderer being created.
    Config base = LoadConfig(iniPath);  // as written in the ini
    FILETIME iniTime = FileTime(iniPath);
    // Presets from the ini's Preset= line. Lock held.
    auto adoptIniPresets = [&] {
        g_activePresets.clear();
        for (auto& n : base.startPresets)
            if (!SetPresetActiveLocked(n, true)) Log("Preset '%s' not found", n.c_str());
    };
    AcquireSRWLockExclusive(&g_knownLock);
    g_baseShared = base;
    adoptIniPresets();
    ReleaseSRWLockExclusive(&g_knownLock);
    // Before the first Derive: it caps cascade resolution by whether the
    // large-slot packer is really installed.
    // The large-slot packer is needed whenever the atlas has room for >2048 slots.
    // allocate() is hooked whenever the atlas is enlarged too, so LightShadowScale
    // can be switched on live later.
    g_lightShadowScale = base.lightShadowScale;
    const bool allocDiag = InstallShadowHooks(base.logShadowAllocs, g_atlasScale >= 2,
                                              base.lightShadowScale > 1 || g_atlasScale >= 2);
    std::string lastPresetLine = base.presetLine;
    Config cfg;  // what gets applied
    bool modOff = false;
    auto derive = [&] {
        AcquireSRWLockExclusive(&g_knownLock);
        cfg = Derive(base);
        modOff = g_modOff;
        ReleaseSRWLockExclusive(&g_knownLock);
    };
    derive();
    auto updatePhotoKeys = [&] {
        snprintf(g_photoKeys, sizeof(g_photoKeys),
                 "Free cam %s | Pause %s | Step %s | Speed %s/%s/%s | Time %s/%s (+Shift: 1 h)",
                 KeyName(base.freeCamKey).c_str(), KeyName(base.pauseKey).c_str(), KeyName(base.stepKey).c_str(),
                 KeyName(base.slowerKey).c_str(), KeyName(base.fasterKey).c_str(),
                 KeyName(base.normalSpeedKey).c_str(), KeyName(base.timeBackwardKey).c_str(),
                 KeyName(base.timeForwardKey).c_str());
    };
    updatePhotoKeys();
    LONG lastUiVersion = g_uiVersion;
    bool relog = false;
    bool toggleDown = false, presetDown = false, freeCamDown = false;
    bool pauseDown = false, stepDown = false, slowerDown = false, fasterDown = false, normalDown = false;
    bool timeFwdDown = false, timeBackDown = false;
    ULONGLONG timeFwdNext = 0, timeBackNext = 0;
    int lastCamMode = g_cameraMode ? *g_cameraMode : -1;
    g_freeCamLook = base.freeCamLook;
    g_freeCamInvertPitch = base.freeCamInvertPitch;
    g_axisDiagOn = base.logFreeCamInput && g_getAxisTrampoline;
    auto setLightScale = [&](int want) {
        if (g_lightShadowScale == want) return;
        if (g_origAllocate) Log("LightShadowScale %ld -> %d", (long)g_lightShadowScale, want);
        else Log("LightShadowScale needs a restart (allocate() was not hooked at startup)");
        g_lightShadowScale = want;
    };

    if (!LocateRegistry()) {
        Log("ERROR: tweakable registry signature not found - unsupported game build. Mod idle.");
        return 0;
    }
    ULONGLONG lastAllocLog = GetTickCount64(), lastAxisLog = lastAllocLog;
    bool loggedFirstPack = false;
    unsigned long long loggedFails = 0;
    auto& known = g_known;
    std::unordered_set<uint8_t*> seen;
    std::unordered_set<std::string> reported;  // overrides we've logged as applied
    int lastCount = -1;
    ULONGLONG countChangedAt = GetTickCount64();
    size_t dumpedCount = 0;
    bool reapplyAll = true;

    for (;;) {
        Sleep(cfg.pollMs);

        FILETIME t = FileTime(iniPath);
        if (CompareFileTime(&t, &iniTime) != 0) {
            iniTime = t;
            Log("awtweaks.ini changed - reloading");
            base = LoadConfig(iniPath);
            AcquireSRWLockExclusive(&g_knownLock);
            g_baseShared = base;
            if (base.presetLine != lastPresetLine) {
                adoptIniPresets();  // the ini's Preset= changed: it wins
            } else {
                // Keep the presets picked at runtime (F9 / overlay), minus any that vanished.
                auto keep = g_activePresets;
                g_activePresets.clear();
                for (auto& n : keep) SetPresetActiveLocked(n, true);
            }
            ReleaseSRWLockExclusive(&g_knownLock);
            lastPresetLine = base.presetLine;
            derive();
            setLightScale(modOff ? 1 : base.lightShadowScale);
            g_freeCamLook = base.freeCamLook;
            g_freeCamInvertPitch = base.freeCamInvertPitch;
            updatePhotoKeys();
            reported.clear();
            reapplyAll = true;
            dumpedCount = 0;  // refresh the dump's "now" markers
        }

        // Hotkeys (only while the game window has focus).
        if (KeyPressed(base.toggleKey, &toggleDown)) {
            AcquireSRWLockExclusive(&g_knownLock);
            g_modOff = !g_modOff;
            bool off = g_modOff;
            ReleaseSRWLockExclusive(&g_knownLock);
            InterlockedIncrement(&g_uiVersion);
            relog = true;
            Log("[%s] A/B: mod %s", KeyName(base.toggleKey).c_str(), off ? "OFF (stock)" : "ON");
            if (base.beep) Beep(off ? 440 : 880, 120);
        }
        if (g_isPressedTrampoline && KeyPressed(base.freeCamKey, &freeCamDown)) {
            QueuePress(kToggleFreeCamera);  // consumed by the game's next TOGGLE_FREECAMERA query
            Log("[%s] Free camera toggle requested (camera mode %d)", KeyName(base.freeCamKey).c_str(),
                g_cameraMode ? *g_cameraMode : -1);
        }
        // Photo mode (only if its globals/hooks were found at startup).
        if (g_paused && KeyPressed(base.pauseKey, &pauseDown)) {
            QueuePress(kPause);
            Log("[%s] Pause toggle (paused=%d)", KeyName(base.pauseKey).c_str(), *g_paused);
        }
        if (g_paused && KeyPressed(base.stepKey, &stepDown)) {
            if (*g_paused) QueuePress(kStepFrame);  // the game only steps while paused
        }
        if (g_timeScaleStep) {
            static const float kLadder[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
            const float cur = *g_timeScale;
            if (KeyPressed(base.slowerKey, &slowerDown)) {
                float next = kLadder[0];
                for (float s : kLadder)
                    if (s < cur - 0.001f) next = s;
                SetTimeScale(next);
                Log("[%s] Game speed %.2fx", KeyName(base.slowerKey).c_str(), next);
            }
            if (KeyPressed(base.fasterKey, &fasterDown)) {
                float next = kLadder[7];
                for (int i = 7; i >= 0; --i)
                    if (kLadder[i] > cur + 0.001f) next = kLadder[i];
                SetTimeScale(next);
                Log("[%s] Game speed %.2fx", KeyName(base.fasterKey).c_str(), next);
            }
            if (KeyPressed(base.normalSpeedKey, &normalDown)) {
                SetTimeScale(1.0f);
                Log("[%s] Game speed 1x", KeyName(base.normalSpeedKey).c_str());
            }
        }
        if (g_paused) {
            // PageUp/PageDown: +-1 minute of game clock (hold to keep going);
            // with Shift: +-1 hour.
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            auto timeKey = [&](int vk, bool* down, ULONGLONG* next, bool forward) {
                bool pressed = KeyPressed(vk, down);
                bool repeat = *down && GameHasFocus() && GetTickCount64() >= *next;
                if (!pressed && !repeat) return;
                *next = GetTickCount64() + (pressed ? 400 : 120);
                if (shift) {
                    if (forward) QueuePress(kTimeForwardHour);
                    else QueuePress(kTimeBackward, 60);  // no TIME_BACKWARDFAST handler
                } else {
                    QueuePress(forward ? kTimeForward : kTimeBackward);
                }
                if (pressed) Log("[%s] Time of day %s1 %s", KeyName(vk).c_str(), forward ? "+" : "-", shift ? "hour" : "minute");
            };
            timeKey(base.timeForwardKey, &timeFwdDown, &timeFwdNext, true);
            timeKey(base.timeBackwardKey, &timeBackDown, &timeBackNext, false);
        }

        if (g_cameraMode && *g_cameraMode != lastCamMode) {
            static const char* kModes[] = {"player", "free camera", "free camera (frozen, Alan controllable)",
                                           "vehicle", "cameraman", "cameraman", "free camera", "cinematic"};
            int m = *g_cameraMode;
            Log("Camera mode -> %d (%s)", m, m >= 0 && m < 8 ? kModes[m] : "?");
            lastCamMode = m;
        }
        if (KeyPressed(base.presetKey, &presetDown)) {
            // Cycles the "Quality" group (none -> first -> next -> ... -> none), or all
            // presets one at a time if no preset declares Group=Quality. Pressing it
            // while the mod is toggled off turns the mod back on first.
            AcquireSRWLockExclusive(&g_knownLock);
            g_modOff = false;
            std::vector<std::string> cycle;
            for (auto& d : base.presets)
                if (Lower(d.group) == "quality") cycle.push_back(d.name);
            const bool all = cycle.empty();
            if (all)
                for (auto& d : base.presets) cycle.push_back(d.name);
            int cur = -1;
            for (int i = 0; i < (int)cycle.size(); ++i)
                for (auto& a : g_activePresets)
                    if (Lower(a) == Lower(cycle[i])) cur = i;
            for (auto& n : cycle) SetPresetActiveLocked(n, false);
            int next = cur + 1 < (int)cycle.size() ? cur + 1 : -1;
            if (next >= 0) SetPresetActiveLocked(cycle[next], true);
            std::string active = JoinActivePresetsLocked();
            ReleaseSRWLockExclusive(&g_knownLock);
            InterlockedIncrement(&g_uiVersion);
            relog = true;
            Log("[%s] Presets: %s (mod ON)", KeyName(base.presetKey).c_str(), active.empty() ? "none" : active.c_str());
            if (base.beep) {
                for (int i = 0; i <= next; ++i) Beep(660, 60), Sleep(40);  // one beep per position
                if (next < 0) Beep(330, 150);
            }
        }

        // Preset benchmark (started from the overlay).
        {
            const ULONGLONG kSettleMs = base.benchSettleMs, kMeasureMs = base.benchMeasureMs;
            AcquireSRWLockExclusive(&g_knownLock);
            auto& b = g_bench;
            auto switchTo = [&](const std::string& quality) {
                for (auto& d : g_baseShared.presets)
                    if (Lower(d.group) == "quality") SetPresetActiveLocked(d.name, false);
                if (!quality.empty()) SetPresetActiveLocked(quality, true);
                g_modOff = false;
                InterlockedIncrement(&g_uiVersion);
            };
            const ULONGLONG now = GetTickCount64();
            if (b.requested && !b.running) {
                b.requested = false;
                b.running = true;
                b.results.clear();
                b.savedPresets = g_activePresets;
                b.stages = {""};
                for (auto& d : g_baseShared.presets)
                    if (Lower(d.group) == "quality") b.stages.push_back(d.name);
                b.stage = 0;
                b.measuring = false;
                b.stageStart = now;
                switchTo(b.stages[0]);
                Log("Benchmark: %zu stages (Stock + Quality presets), %.1f s each", b.stages.size(),
                    (kSettleMs + kMeasureMs) / 1000.0);
            } else if (b.running) {
                if (!b.measuring && now - b.stageStart >= kSettleMs) {
                    b.measuring = true;
                    b.framesAtMeasure = g_frameCount;
                } else if (b.measuring && now - b.stageStart >= kSettleMs + kMeasureMs) {
                    const double secs = (now - b.stageStart - kSettleMs) / 1000.0;
                    const float fps = (float)((g_frameCount - b.framesAtMeasure) / secs);
                    const std::string name = b.stages[b.stage].empty() ? "Stock" : b.stages[b.stage];
                    b.results.push_back({name, fps});
                    Log("Benchmark: %-12s %.1f fps", name.c_str(), fps);
                    if (++b.stage < (int)b.stages.size()) {
                        b.measuring = false;
                        b.stageStart = now;
                        switchTo(b.stages[b.stage]);
                    } else {
                        b.running = false;
                        g_activePresets.clear();
                        for (auto& n : b.savedPresets) SetPresetActiveLocked(n, true);
                        InterlockedIncrement(&g_uiVersion);
                        Log("Benchmark done; presets restored: %s", JoinActivePresetsLocked().c_str());
                    }
                }
            }
            ReleaseSRWLockExclusive(&g_knownLock);
        }

        if (g_uiVersion != lastUiVersion) {  // overlay edit/reset/save, preset or A/B change
            lastUiVersion = g_uiVersion;
            derive();
            setLightScale(modOff ? 1 : base.lightShadowScale);
            if (relog) reported.clear();  // log what a preset/A-B switch applies, not every slider drag
            relog = false;
            reapplyAll = true;
        }

        int count = *g_registryCount;
        if (count < 0 || count > kMaxTweakables) continue;

        AcquireSRWLockExclusive(&g_knownLock);

        // Pick up newly registered tweakables. Objects are function-local statics,
        // so their addresses stay valid for the life of the process.
        std::vector<size_t> fresh;
        for (int i = 0; i < count; ++i) {
            uint8_t* obj = g_registry[i];
            if (!obj || seen.count(obj)) continue;
            const char* name = TweakName(obj);
            if (!name) continue;
            seen.insert(obj);
            Known k{obj, name, TweakType(obj), {}, false};
            if (k.type == kList) memcpy(k.original, obj + kOffListIndex, 4);
            else memcpy(k.original, obj + kOffValue, sizeof(k.original));
            if (k.type == kFloat && k.name == kPackedName) memcpy(&g_stockCascadeFilterSize, k.original, 4);
            g_knownIndex[k.name] = known.size();
            known.push_back(std::move(k));
            fresh.push_back(known.size() - 1);
        }

        // Overrides removed from the ini (or mod disabled) go back to the engine's value.
        if (reapplyAll) {
            for (auto& k : known)
                if (k.touched && (!cfg.enabled || !cfg.tweaks.count(k.name))) Restore(k);
        }

        if (cfg.enabled) {
            auto applyOne = [&](Known& k) {
                auto it = cfg.tweaks.find(k.name);
                if (it == cfg.tweaks.end()) return;
                bool first = reported.insert(k.name).second;
                bool changed = Apply(k, it->second, cfg, first);
                if (changed) k.touched = true;
                if (first && !changed) {
                    const char* shown = it->second.raw.c_str();
                    if (k.type == kList)  // the option actually selected, not what was typed
                        if (const char* item = ListItem(k.obj, ListIndex(k.obj))) shown = item;
                    Log("  %s already = %s", k.name.c_str(), shown);
                }
            };
            if (reapplyAll || cfg.enforce) {
                for (auto& k : known) applyOne(k);
            } else {
                for (size_t i : fresh) applyOne(known[i]);
            }
        }
        reapplyAll = false;
        ReleaseSRWLockExclusive(&g_knownLock);

        if (!fresh.empty()) Log("Registered tweakables: %zu (+%zu)", known.size(), fresh.size());

        if (g_axisDiagOn && GetTickCount64() - lastAxisLog >= 2000) {
            lastAxisLog = GetTickCount64();
            LogAxisDiag();
        }
        if (GetTickCount64() - lastAllocLog >= 5000) {
            lastAllocLog = GetTickCount64();
            if (allocDiag) LogAllocStats();
            // Always report the large-slot packer engaging or failing, even without diagnostics.
            AcquireSRWLockShared(&g_allocLock);
            unsigned long long packs = g_ourPacks, fails = g_ourPackFailures;
            ReleaseSRWLockShared(&g_allocLock);
            if (packs && !loggedFirstPack) {
                Log("Large-slot packer active (cascades above 2048)");
                loggedFirstPack = true;
            }
            if (fails > loggedFails) {
                Log("WARNING: large-slot packer could not place %llu frame(s) - atlas too small for the "
                    "requested cascade resolution", fails - loggedFails);
                loggedFails = fails;
            }
        }

        // Dump once the registry has been stable for ~3 s and has grown since last dump.
        if (count != lastCount) countChangedAt = GetTickCount64();
        lastCount = count;
        if (cfg.dumpAll && known.size() != dumpedCount && GetTickCount64() - countChangedAt >= 3000) {
            AcquireSRWLockShared(&g_knownLock);
            Dump(known, dumpPath);
            ReleaseSRWLockShared(&g_knownLock);
            dumpedCount = known.size();
            Log("Wrote awtweaks_dump.txt (%zu entries)", dumpedCount);
            for (auto& [name, o] : cfg.tweaks)
                if (!reported.count(name)) Log("  pending (not registered yet): %s", name.c_str());
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Exports (see xinput1_4.def)

#define FORWARD(fn, id)                                                                         \
    extern "C" uintptr_t WINAPI fn(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,          \
                                   uintptr_t e, uintptr_t f) {                                  \
        static Fwd real;                                                                        \
        Fwd p = Resolve(id, &real);                                                             \
        if (!p || t_depth) return ERROR_DEVICE_NOT_CONNECTED;                                   \
        ++t_depth;                                                                              \
        uintptr_t r = p(a, b, c, d, e, f);                                                      \
        --t_depth;                                                                              \
        return r;                                                                               \
    }

FORWARD(XInputGetState, "XInputGetState")
FORWARD(XInputSetState, "XInputSetState")
FORWARD(XInputGetCapabilities, "XInputGetCapabilities")
FORWARD(XInputGetBatteryInformation, "XInputGetBatteryInformation")
FORWARD(XInputGetKeystroke, "XInputGetKeystroke")
FORWARD(XInputGetAudioDeviceIds, "XInputGetAudioDeviceIds")
FORWARD(XInputOrd100, MAKEINTRESOURCEA(100))
FORWARD(XInputOrd101, MAKEINTRESOURCEA(101))
FORWARD(XInputOrd102, MAKEINTRESOURCEA(102))
FORWARD(XInputOrd103, MAKEINTRESOURCEA(103))
FORWARD(XInputOrd104, MAKEINTRESOURCEA(104))
FORWARD(XInputOrd108, MAKEINTRESOURCEA(108))
FORWARD(XInputOrd109, MAKEINTRESOURCEA(109))

extern "C" void WINAPI XInputEnable(BOOL enable) {
    static Fwd real;
    Fwd p = Resolve("XInputEnable", &real);
    if (!p || t_depth) return;
    ++t_depth;
    p(enable, 0, 0, 0, 0, 0);
    --t_depth;
}

// ---------------------------------------------------------------------------
// Overlay API (see awtweaks_api.h; used by the ReShade add-on)

namespace {

std::string FormatValue(const Known* k, int type, const float* v) {
    char buf[96];
    if (type == kList && k)
        if (const char* item = ListItem(k->obj, (int)v[0])) return item;
    if (type == kBool) return v[0] != 0.0f ? "true" : "false";
    if (type == kColor) snprintf(buf, sizeof(buf), "%g, %g, %g, %g", v[0], v[1], v[2], v[3]);
    else snprintf(buf, sizeof(buf), "%g", v[0]);
    return buf;
}

// Rewrites `key=value` lines in place (uncommenting ";key=" if that's all there is),
// or appends to the end of the section. Keeps any trailing "; comment".
bool WriteIniValues(const std::wstring& path,
                    const std::vector<std::tuple<std::string, std::string, std::string>>& items) {  // section, key, value
    std::vector<std::string> lines;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[2048];
        while (fgets(buf, sizeof(buf), f)) {
            std::string s = buf;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            lines.push_back(s);
        }
        fclose(f);
    } else {
        return false;
    }
    for (auto& [section, key, value] : items) {
        const std::string want = Lower(section);
        int secStart = -1, secEnd = (int)lines.size(), exact = -1, commented = -1;
        for (int i = 0; i < (int)lines.size(); ++i) {
            std::string t = Trim(lines[i]);
            if (!t.empty() && t[0] == '[') {
                if (secStart >= 0) {
                    secEnd = i;
                    break;
                }
                if (Lower(Trim(t.substr(1, t.find(']') - 1))) == want) secStart = i;
                continue;
            }
            if (secStart < 0) continue;
            bool isComment = !t.empty() && (t[0] == ';' || t[0] == '#');
            std::string body = isComment ? Trim(t.substr(1)) : t;
            size_t c = body.find_first_of(";#");  // ignore trailing comment when finding '='
            size_t eq = body.substr(0, c).rfind('=');
            if (eq == std::string::npos || Trim(body.substr(0, eq)) != key) continue;
            if (!isComment) exact = i;
            else if (commented < 0) commented = i;
        }
        std::string line = key + "=" + value;
        int at = exact >= 0 ? exact : commented;
        if (at >= 0) {
            size_t c = lines[at].find(';', lines[at].find('=') + 1);
            if (exact >= 0 && c != std::string::npos) line += "    " + lines[at].substr(c);
            lines[at] = line;
        } else if (secStart >= 0) {
            int ins = secEnd;
            while (ins - 1 > secStart && Trim(lines[ins - 1]).empty()) --ins;  // before trailing blanks
            lines.insert(lines.begin() + ins, line);
        } else {
            lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(line);
        }
    }
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    for (auto& l : lines) fprintf(f, "%s\r\n", l.c_str());
    fclose(f);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

}  // namespace

extern "C" int __cdecl AWT_GetStatus(AwtStatus* out) {
    if (!out) return 0;
    AcquireSRWLockShared(&g_knownLock);
    memset(out, 0, sizeof(*out));
    out->apiVersion = AWT_API_VERSION;
    out->modOff = g_modOff;
    out->overlayEdits = (int)g_ui.size();
    out->registered = (int)g_known.size();
    snprintf(out->preset, sizeof(out->preset), "%s", JoinActivePresetsLocked().c_str());
    out->sunSize = g_sunSize;
    out->contactShadows = g_contactShadows;
    out->shadowFxSource = g_modOff ? AWT_SRC_NONE : g_shadowFxSource;
    ReleaseSRWLockShared(&g_knownLock);
    return 1;
}

extern "C" int __cdecl AWT_Snapshot(AwtInfo* out, int cap) {
    AcquireSRWLockShared(&g_knownLock);
    const int n = (int)g_known.size();
    for (int i = 0; out && i < n && i < cap; ++i) {
        const Known& k = g_known[i];
        AwtInfo& o = out[i];
        snprintf(o.name, sizeof(o.name), "%s", k.name.c_str());
        o.type = k.type;
        auto src = g_sourceOf.find(k.name);
        o.source = src == g_sourceOf.end() || g_modOff ? AWT_SRC_NONE : src->second;
        const uint8_t* v = k.obj + kOffValue;
        memset(o.value, 0, sizeof(o.value));
        memset(o.original, 0, sizeof(o.original));
        o.min = o.max = 0;
        if (k.type == kBool) {
            o.value[0] = v[0] ? 1.0f : 0.0f;
            o.original[0] = k.original[0] ? 1.0f : 0.0f;
        } else if (k.type == kFloat) {
            const float* f = reinterpret_cast<const float*>(v);
            o.value[0] = f[0];
            o.min = f[1];
            o.max = f[2];
            memcpy(o.original, k.original, 4);
        } else if (k.type == kColor) {
            memcpy(o.value, v, 16);
            memcpy(o.original, k.original, 16);
        } else if (k.type == kList) {
            int orig;
            memcpy(&orig, k.original, 4);
            o.value[0] = (float)ListIndex(k.obj);
            o.original[0] = (float)orig;
            o.max = (float)(ListCount(k.obj) - 1);
        }
        o.items[0] = 0;
        if (k.type == kList) {
            // Option names, newline-separated, as many as fit.
            size_t used = 0;
            for (int j = 0; j < ListCount(k.obj); ++j) {
                const char* item = ListItem(k.obj, j);
                size_t len = item ? strlen(item) : 0;
                if (used + len + 2 > sizeof(o.items)) break;
                memcpy(o.items + used, item, len);
                used += len;
                o.items[used++] = '\n';
                o.items[used] = 0;
            }
        }
    }
    ReleaseSRWLockShared(&g_knownLock);
    return n;
}

extern "C" int __cdecl AWT_Set(const char* name, const float* v) {
    if (!name || !v) return 0;
    AcquireSRWLockExclusive(&g_knownLock);
    auto it = g_knownIndex.find(name);
    int ok = 0;
    if (IsShadowFxName(name)) {  // not engine tweakables: the worker packs them on re-derive
        Override o;
        o.f[0] = (std::max)(0.0f, v[0]);
        o.count = 1;
        o.raw = FormatValue(nullptr, kFloat, o.f);
        g_ui[Lower(name) == Lower(kSunSizeName) ? kSunSizeName : kContactShadowsName] = o;
        ok = 1;
    } else if (it != g_knownIndex.end()) {
        Known& k = g_known[it->second];
        Override o;
        o.count = k.type == kColor ? 4 : 1;
        memcpy(o.f, v, sizeof(float) * o.count);
        o.isBool = k.type == kBool;
        o.b = v[0] != 0.0f;
        if (k.type == kFloat) o.f[0] = SafeCascadeResolution(k.name, o.f[0]);
        if (k.type == kFloat && k.name == kPackedName) o.f[0] = RoundFilterSize(o.f[0]);
        o.raw = FormatValue(&k, k.type, o.f);
        if (k.type == kBool || k.type == kFloat || k.type == kColor || k.type == kList) {
            g_ui[k.name] = o;
            if (!g_modOff) {  // show it this frame; the worker keeps enforcing it
                Config c;
                c.clampToRange = false;
                if (Apply(k, o, c, false)) k.touched = true;
            }
            ok = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_knownLock);
    if (ok) InterlockedIncrement(&g_uiVersion);
    return ok;
}

extern "C" int __cdecl AWT_Reset(const char* name) {
    AcquireSRWLockExclusive(&g_knownLock);
    int n = 0;
    if (!name) {
        n = (int)g_ui.size();
        g_ui.clear();
    } else {
        n = (int)g_ui.erase(name);
    }
    ReleaseSRWLockExclusive(&g_knownLock);
    if (n) InterlockedIncrement(&g_uiVersion);  // worker re-derives: restores or reapplies ini values
    return n;
}

extern "C" int __cdecl AWT_SaveToIni(void) {
    std::vector<std::tuple<std::string, std::string, std::string>> items;
    AcquireSRWLockExclusive(&g_knownLock);
    for (auto& [name, o] : g_ui) {
        auto it = g_knownIndex.find(name);
        const Known* kp = it != g_knownIndex.end() ? &g_known[it->second] : nullptr;
        int type = kp ? kp->type : kFloat;
        // A key the active preset sets would be shadowed in [Tweaks], so save it there.
        auto owner = g_presetOf.find(name);  // the active preset whose value it replaced
        items.emplace_back(owner != g_presetOf.end() ? "Preset:" + owner->second : "Tweaks", name,
                           FormatValue(kp, type, o.f));
    }
    ReleaseSRWLockExclusive(&g_knownLock);
    if (items.empty()) return 0;
    if (!WriteIniValues(g_dir + L"awtweaks.ini", items)) {
        Log("Overlay: saving to awtweaks.ini failed");
        return -1;
    }
    AcquireSRWLockExclusive(&g_knownLock);
    g_ui.clear();  // the ini now carries them; the worker reloads it on the mtime change
    ReleaseSRWLockExclusive(&g_knownLock);
    InterlockedIncrement(&g_uiVersion);
    Log("Overlay: saved %zu value(s) to awtweaks.ini", items.size());
    return (int)items.size();
}

extern "C" int __cdecl AWT_GetPresets(AwtPreset* out, int cap) {
    AcquireSRWLockShared(&g_knownLock);
    const int n = (int)g_baseShared.presets.size();
    for (int i = 0; out && i < n && i < cap; ++i) {
        const auto& d = g_baseShared.presets[i];
        AwtPreset& o = out[i];
        snprintf(o.name, sizeof(o.name), "%s", d.name.c_str());
        snprintf(o.group, sizeof(o.group), "%s", d.group.c_str());
        snprintf(o.description, sizeof(o.description), "%s", d.description.c_str());
        o.active = 0;
        for (auto& a : g_activePresets) o.active |= Lower(a) == Lower(d.name);
    }
    ReleaseSRWLockShared(&g_knownLock);
    return n;
}

extern "C" int __cdecl AWT_SetPresetActive(const char* name, int active) {
    if (!name) return 0;
    AcquireSRWLockExclusive(&g_knownLock);
    int ok = SetPresetActiveLocked(name, active != 0);
    if (ok && active) g_modOff = false;  // picking a preset implies wanting the mod on
    ReleaseSRWLockExclusive(&g_knownLock);
    if (ok) {
        InterlockedIncrement(&g_uiVersion);
        Log("Overlay: preset '%s' %s", name, active ? "on" : "off");
    }
    return ok;
}

extern "C" int __cdecl AWT_SetModOn(int on) {
    AcquireSRWLockExclusive(&g_knownLock);
    g_modOff = !on;
    ReleaseSRWLockExclusive(&g_knownLock);
    InterlockedIncrement(&g_uiVersion);
    Log("Overlay: mod %s", on ? "ON" : "OFF (stock)");
    return 1;
}

// Writes the active presets as the ini's startup Preset= line.
extern "C" int __cdecl AWT_SaveStartupPresets(void) {
    AcquireSRWLockShared(&g_knownLock);
    std::string line = JoinActivePresetsLocked();
    ReleaseSRWLockShared(&g_knownLock);
    if (line.empty()) line = "None";
    if (!WriteIniValues(g_dir + L"awtweaks.ini", {{"Settings", "Preset", line}})) return 0;
    Log("Overlay: startup presets saved: %s", line.c_str());
    return 1;
}

extern "C" int __cdecl AWT_GetPhoto(AwtPhoto* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->freeCamAvailable = g_cameraMode != nullptr && g_isPressedTrampoline != nullptr;
    out->photoAvailable = g_paused != nullptr && g_timeScaleStep != nullptr && g_isPressedTrampoline != nullptr;
    out->cameraMode = g_cameraMode ? *g_cameraMode : 0;
    out->paused = g_paused ? *g_paused : 0;
    out->timeScale = g_timeScale ? *g_timeScale : 1.0f;
    snprintf(out->keys, sizeof(out->keys), "%s", g_photoKeys);
    return 1;
}

extern "C" int __cdecl AWT_PhotoAction(int action) {
    if (!g_isPressedTrampoline) return 0;
    switch (action) {
        case AWT_PHOTO_FREECAM: if (!g_cameraMode) return 0; QueuePress(kToggleFreeCamera); break;
        case AWT_PHOTO_PAUSE: if (!g_paused) return 0; QueuePress(kPause); break;
        case AWT_PHOTO_STEP: if (!g_paused || !*g_paused) return 0; QueuePress(kStepFrame); break;
        case AWT_PHOTO_TIME_MINUS_HOUR: QueuePress(kTimeBackward, 60); break;
        case AWT_PHOTO_TIME_MINUS_MINUTE: QueuePress(kTimeBackward); break;
        case AWT_PHOTO_TIME_PLUS_MINUTE: QueuePress(kTimeForward); break;
        case AWT_PHOTO_TIME_PLUS_HOUR: QueuePress(kTimeForwardHour); break;
        default: return 0;
    }
    return 1;
}

extern "C" int __cdecl AWT_StartBenchmark(void) {
    AcquireSRWLockExclusive(&g_knownLock);
    int ok = !g_bench.running && g_largeSlotFix;  // needs the repack hook to count frames
    if (ok) g_bench.requested = true;
    ReleaseSRWLockExclusive(&g_knownLock);
    return ok;
}

extern "C" int __cdecl AWT_GetBenchmark(AwtBenchmark* out) {
    if (!out) return 0;
    AcquireSRWLockShared(&g_knownLock);
    memset(out, 0, sizeof(*out));
    out->available = g_largeSlotFix;
    out->running = g_bench.running || g_bench.requested;
    out->stage = g_bench.stage;
    out->stages = (int)g_bench.stages.size();
    if (g_bench.running && g_bench.stage >= 0 && g_bench.stage < (int)g_bench.stages.size())
        snprintf(out->current, sizeof(out->current), "%s",
                 g_bench.stages[g_bench.stage].empty() ? "Stock" : g_bench.stages[g_bench.stage].c_str());
    out->resultCount = (int)(std::min)(g_bench.results.size(), (size_t)8);
    for (int i = 0; i < out->resultCount; ++i) {
        snprintf(out->results[i].name, sizeof(out->results[i].name), "%s", g_bench.results[i].first.c_str());
        out->results[i].fps = g_bench.results[i].second;
    }
    ReleaseSRWLockShared(&g_knownLock);
    return 1;
}

extern "C" int __cdecl AWT_SetTimeScale(float scale) {
    if (!g_timeScaleStep) return 0;
    SetTimeScale(scale);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(inst, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            g_dir = path;
            g_dir.resize(g_dir.find_last_of(L"\\/") + 1);
        }
        // Keep the previous run's log so a crash on startup can still be diagnosed.
        MoveFileExW((g_dir + L"awtweaks.log").c_str(), (g_dir + L"awtweaks.prev.log").c_str(),
                    MOVEFILE_REPLACE_EXISTING);
        g_log = _wfopen((g_dir + L"awtweaks.log").c_str(), L"w");
        Log("AWTweaks " AWT_VERSION_STRING " loaded (made for Alan Wake Remastered build " AWT_TESTED_GAME_BUILD ")");
        // 2 = 16384x8192, D3D12's max texture size; anything larger can't be created.
        UINT scale = GetPrivateProfileIntW(L"Shadows", L"AtlasScale", 1, (g_dir + L"awtweaks.ini").c_str());
        if (scale > 2) {
            Log("AtlasScale=%u too large (max 2); using 2", scale);
            scale = 2;
        }
        if (PatchAtlasSize((int)scale)) g_atlasScale = (int)scale;
        // Before the game's own code runs, so no thread can be inside getAxis/isPressed.
        auto iniBool = [&](const wchar_t* section) {
            wchar_t v[16] = {};
            GetPrivateProfileStringW(section, L"Enabled", L"false", v, 16, (g_dir + L"awtweaks.ini").c_str());
            return !_wcsnicmp(v, L"true", 4) || !_wcsnicmp(v, L"1", 1) || !_wcsnicmp(v, L"on", 2) ||
                   !_wcsnicmp(v, L"yes", 3);
        };
        const bool freeCam = iniBool(L"FreeCamera"), photo = iniBool(L"PhotoMode");
        if (freeCam) InstallFreeCameraHook();
        if (freeCam || photo) InstallTogglePressHook();
        if (photo) LocatePhotoState();
        if (HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(h);
    }
    return TRUE;
}
