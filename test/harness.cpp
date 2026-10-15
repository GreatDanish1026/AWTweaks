// Offline test for AWTweaks: fakes the engine's tweakable registry (same ctor
// byte signature, same object layout), loads the proxy DLL and checks overrides.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/awtweaks_api.h"

static uint8_t* g_reg[0x500];
static int g_cnt;

// Copy of the engine's registration snippet; disp32s are patched at startup.
__attribute__((section(".text"), used)) static uint8_t g_sig[] = {
    0x48, 0x63, 0x0D, 0, 0, 0, 0,              // movsxd rcx, [count]
    0x81, 0xF9, 0x00, 0x05, 0x00, 0x00,        // cmp ecx, 500h
    0x7D, 0x13,                                // jge
    0x48, 0x8D, 0x15, 0, 0, 0, 0,              // lea rdx, [array]
    0x48, 0x89, 0x3C, 0xCA, 0xFF, 0xC1, 0x89, 0x0D, 0, 0, 0, 0, 0xC3};

// Fake getAxis(int control): the game's prologue (cmp qword [rip+X],0 / je),
// then returns (float)control, or 0 when *X == 0. Displacements patched at startup.
static void* volatile g_fakeInputReady = (void*)1;
__attribute__((section(".text"), used)) static uint8_t g_getAxis[] = {
    0x48, 0x83, 0xEC, 0x38,                          // sub rsp, 38h
    0x48, 0x83, 0x3D, 0, 0, 0, 0, 0x00,              // cmp qword ptr [rip+X], 0
    0x4C, 0x63, 0xC1,                                // movsxd r8, ecx
    0x0F, 0x84, 0x0A, 0x00, 0x00, 0x00,              // je zero
    0xF3, 0x49, 0x0F, 0x2A, 0xC0,                    // cvtsi2ss xmm0, r8
    0x48, 0x83, 0xC4, 0x38, 0xC3,                    // add rsp, 38h; ret
    0x0F, 0x57, 0xC0, 0x48, 0x83, 0xC4, 0x38, 0xC3}; // zero: xorps; add rsp; ret
// Fake isPressed(int control): the game's first 32 bytes (cmp/je/movsxd/mov/mov r9)
// with displacements pointed at harmless data, then returns control == 5.
static void* volatile g_fakePressReady = (void*)1;
static volatile int g_zeroInt = 0;
static uint64_t g_fakeTableData[4] = {};
static uint64_t* volatile g_fakeTable = g_fakeTableData;
__attribute__((section(".text"), used)) static uint8_t g_isPressed[] = {
    0x48, 0x83, 0x3D, 0, 0, 0, 0, 0x00,    // cmp qword ptr [rip+X], 0
    0x0F, 0x84, 0x19, 0x00, 0x00, 0x00,    // je early (+25 -> offset 39)
    0x48, 0x63, 0x15, 0, 0, 0, 0,          // movsxd rdx, [rip+zeroInt]
    0x48, 0x8B, 0x05, 0, 0, 0, 0,          // mov rax, [rip+fakeTable]
    0x4C, 0x8B, 0x0C, 0xD0,                // mov r9, [rax+rdx*8]
    0x83, 0xF9, 0x05,                      // cmp ecx, 5
    0x0F, 0x94, 0xC0,                      // sete al
    0xC3,                                  // ret
    0x30, 0xC0, 0xC3};                     // early: xor al, al; ret

// Fake photo-mode state + copies of the code the DLL finds it through.
struct FakeTimeScale { volatile int step; volatile float scale; };
static FakeTimeScale g_fakeTs = {100, 1.0f};
static volatile uint8_t g_fakePaused = 0, g_fakeStepping = 0;
__attribute__((section(".text"), used)) static uint8_t g_tsRef[] = {
    0x8B, 0x05, 0, 0, 0, 0, 0x83, 0xF8, 0x0A, 0x7E, 0x05, 0x83, 0xE8, 0x0A, 0xEB, 0x0F, 0xC3};
__attribute__((section(".text"), used)) static uint8_t g_pausedRef[] = {
    0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x75, 0x09, 0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x74, 0x35,
    0x33, 0xD2, 0xB9, 0x87, 0x00, 0x00, 0x00, 0xC3};

// Copy of the free-camera toggle's mode read: movsxd rax,[mode]; cmp eax,1; jne; call; neg; sbb.
static volatile int g_fakeMode = 0;
__attribute__((section(".text"), used)) static uint8_t g_modeRef[] = {
    0x48, 0x63, 0x05, 0, 0, 0, 0, 0x83, 0xF8, 0x01, 0x75, 0x1B, 0xE8, 0, 0, 0, 0,
    0x48, 0xF7, 0xD8, 0x1B, 0xF6, 0xC3};

// Copy of the allocator ctor's atlas-size stores (patched by [Shadows] AtlasScale).
__attribute__((section(".text"), used)) static uint8_t g_atlasCtor[] = {
    0xC7, 0x47, 0x10, 0x00, 0x10, 0x00, 0x00, 0xC7, 0x47, 0x14, 0x00, 0x08, 0x00, 0x00,
    0x88, 0x5F, 0x18, 0xC3};

// Fake rend::ShadowMapAllocator::allocate: the game's exact prologue/epilogue,
// body returns self, or nullptr when w == 999.
__attribute__((section(".text"), used)) static uint8_t g_allocate[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
    0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xE9, 0x41, 0x8B,
    0xF0, 0x44, 0x8B, 0xF2, 0x48, 0x8B, 0xF9,                    // prologue
    0x31, 0xC0,                                                  // xor eax, eax
    0x41, 0x81, 0xFE, 0xE7, 0x03, 0x00, 0x00,                    // cmp r14d, 999
    0x74, 0x03,                                                  // je +3
    0x48, 0x8B, 0xC7,                                            // mov rax, rdi
    0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x8B, 0x6C, 0x24, 0x38, 0x48, 0x8B, 0x74, 0x24, 0x40, 0x48,
    0x8B, 0x7C, 0x24, 0x48, 0x48, 0x83, 0xC4, 0x20, 0x41, 0x5E, 0xC3};  // epilogue
// Fake repack(this): game prologue, then halves packer entry 1 (a "light").
__attribute__((section(".text"), used)) static uint8_t g_repack[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30,
    0x48, 0x8B, 0xF1, 0x48, 0x8B, 0x49, 0x30,  // prologue (rcx = packer)
    0x48, 0x8B, 0x41, 0x10,                    // mov rax, [rcx+10h]  entries
    0xD1, 0x78, 0x1C,                          // sar dword [rax+1Ch], 1   entry1.w
    0xD1, 0x78, 0x20,                          // sar dword [rax+20h], 1   entry1.h
    0x48, 0x8B, 0x5C, 0x24, 0x40, 0x48, 0x8B, 0x74, 0x24, 0x48, 0x48, 0x83, 0xC4, 0x30, 0x5F, 0xC3};
static void* const g_allocVtable[] = {nullptr, (void*)g_allocate, nullptr, (void*)g_repack};

struct FakePacker {
    int W, H;
    uint8_t pad[8];
    uint8_t* entries;
    size_t count;
};
struct FakeRegion {
    uint8_t pad[0x30];
    int handle, x, y, w, h;
};
struct FakeAllocator {
    void* const* vt;
    uint8_t pad[8];
    int w, h;
    uint8_t large;
};
using AllocateFn = void*(__fastcall*)(void*, int, int, uint8_t);

struct alignas(16) Obj {
    uint8_t raw[0xE0];  // lists keep their selected index at +0xD0
};
static Obj objs[8];
static char names[8][128];

static uint8_t* Make(int i, const char* name, int type) {
    memset(&objs[i], 0, sizeof(Obj));
    strcpy(names[i], name);
    *(char**)(objs[i].raw + 0x00) = names[i];
    *(int*)(objs[i].raw + 0xA8) = type;
    return objs[i].raw;
}
static float* F(int i) { return (float*)(objs[i].raw + 0xB0); }

static void PatchSig() {
    DWORD old;
    VirtualProtect(g_sig, sizeof(g_sig), PAGE_EXECUTE_READWRITE, &old);
    int32_t d1 = (int32_t)((uint8_t*)&g_cnt - (g_sig + 7));
    int32_t d2 = (int32_t)((uint8_t*)g_reg - (g_sig + 22));
    memcpy(g_sig + 3, &d1, 4);
    memcpy(g_sig + 18, &d2, 4);
    VirtualProtect(g_sig, sizeof(g_sig), old, &old);

    VirtualProtect(g_getAxis, sizeof(g_getAxis), PAGE_EXECUTE_READWRITE, &old);
    int32_t d3 = (int32_t)((uint8_t*)&g_fakeInputReady - (g_getAxis + 12));
    memcpy(g_getAxis + 7, &d3, 4);
    VirtualProtect(g_getAxis, sizeof(g_getAxis), old, &old);
    VirtualProtect(g_isPressed, sizeof(g_isPressed), PAGE_EXECUTE_READWRITE, &old);
    int32_t e1 = (int32_t)((uint8_t*)&g_fakePressReady - (g_isPressed + 8));
    int32_t e2 = (int32_t)((uint8_t*)&g_zeroInt - (g_isPressed + 21));
    int32_t e3 = (int32_t)((uint8_t*)&g_fakeTable - (g_isPressed + 28));
    memcpy(g_isPressed + 3, &e1, 4);
    memcpy(g_isPressed + 17, &e2, 4);
    memcpy(g_isPressed + 24, &e3, 4);
    VirtualProtect(g_isPressed, sizeof(g_isPressed), old, &old);
    VirtualProtect(g_tsRef, sizeof(g_tsRef), PAGE_EXECUTE_READWRITE, &old);
    int32_t p1 = (int32_t)((uint8_t*)&g_fakeTs.step - (g_tsRef + 6));
    memcpy(g_tsRef + 2, &p1, 4);
    VirtualProtect(g_tsRef, sizeof(g_tsRef), old, &old);
    VirtualProtect(g_pausedRef, sizeof(g_pausedRef), PAGE_EXECUTE_READWRITE, &old);
    int32_t p2 = (int32_t)((uint8_t*)&g_fakePaused - (g_pausedRef + 7));
    int32_t p3 = (int32_t)((uint8_t*)&g_fakeStepping - (g_pausedRef + 16));
    memcpy(g_pausedRef + 2, &p2, 4);
    memcpy(g_pausedRef + 11, &p3, 4);
    VirtualProtect(g_pausedRef, sizeof(g_pausedRef), old, &old);
    VirtualProtect(g_modeRef, sizeof(g_modeRef), PAGE_EXECUTE_READWRITE, &old);
    int32_t d4 = (int32_t)((uint8_t*)&g_fakeMode - (g_modeRef + 7));
    memcpy(g_modeRef + 3, &d4, 4);
    VirtualProtect(g_modeRef, sizeof(g_modeRef), old, &old);
}

static int fails = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (c) printf("PASS  %s\n", #c);                            \
        else { printf("FAIL  %s  (line %d)\n", #c, __LINE__); ++fails; } \
    } while (0)

static void WriteIni(const std::wstring& path, const char* text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    fputs(text, f);
    fclose(f);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    std::wstring dir = argv[1];  // folder holding xinput1_4.dll
    PatchSig();

    uint8_t* a = Make(0, "Shadows:Ultra Cascade 4 Cull Radius", 1);
    F(0)[0] = 0.6f; F(0)[1] = 0; F(0)[2] = 2;
    uint8_t* b = Make(1, "Fog:Half Res Mode", 0);
    objs[1].raw[0xB0] = 1;
    uint8_t* c = Make(2, "Volumetric Light:Color", 3);
    F(2)[0] = F(2)[1] = F(2)[2] = F(2)[3] = 1;
    uint8_t* d = Make(3, "HBAO:Bias", 1);
    F(3)[0] = 0.1f; F(3)[1] = 0; F(3)[2] = 1;
    g_reg[0] = a; g_reg[1] = b; g_reg[2] = c; g_reg[3] = d;
    g_cnt = 4;

    WriteIni(dir + L"\\awtweaks.ini",
             "\xEF\xBB\xBF[Settings]\nEnforce=true\nClampToRange=false\nPollMs=20\n"
             "Preset=dark test\nToggleKey=F11\nPresetKey=0x7B\nBeep=false\n"
             "[Debug]\nLogShadowAllocs=true\nBenchmarkSettleMs=60\nBenchmarkMeasureMs=250\n[Shadows]\nAtlasScale=2 ; comment\nLightShadowScale=2\n"
             "[FreeCamera]\nEnabled=true\nLookSensitivity=2\nInvertPitch=true\n"
             "[PhotoMode]\nEnabled=true\nPauseKey=Pause\nSlowerKey=NumpadMinus\n"
             "[Tweaks]\nShadows:Ultra Cascade 4 Cull Radius = 0.3 ; comment\n"
             "Fog:Half Res Mode=false\nVolumetric Light:Color=0.5, 0.25, 0.125\n"
             "HBAO:Bias=5\nHBAO:Radius In Meters=80\n"
             "[Preset:Other]\nGroup=Quality\nDescription=Other one; with a semicolon\nHBAO:Radius In Meters=70\n"
             "[Preset:Dark Test]\nGroup=quality\nHBAO:Radius In Meters=90 ; preset beats [Tweaks]\n"
             "[Preset:Look1]\nGroup=Look\nHBAO:Bias=0.2\n");

    HMODULE m = LoadLibraryW((dir + L"\\xinput1_4.dll").c_str());
    CHECK(m != nullptr);
    Sleep(800);

    // Free camera: getAxis detoured. Normal mode passes through; free-camera mode
    // remaps 9..12 from the player's axes and zeroes the player's controls.
    {
        auto getAxis = (float(__fastcall*)(int, int))(void*)(uint8_t* volatile)g_getAxis;
        CHECK(((volatile uint8_t*)g_getAxis)[0] == 0xFF);  // jmp patched in
        g_fakeMode = 0;
        CHECK(getAxis(1, 0) == 1.0f && getAxis(9, 0) == 9.0f && getAxis(5, 0) == 5.0f);
        g_fakeMode = 1;
        CHECK(getAxis(10, 0) == 1.0f);    // FREECAMERA_Z <- MOVEMENT_Y
        CHECK(getAxis(11, 0) == 10.0f);   // HEADING <- CAMERA_X * LookSensitivity 2
        CHECK(getAxis(12, 0) == -12.0f);  // PITCH <- CAMERA_Y * 2, inverted
        CHECK(getAxis(5, 0) == 0.0f && getAxis(1, 0) == 0.0f && getAxis(3, 0) == 0.0f);  // Alan blocked
        CHECK(getAxis(20, 0) == 20.0f);   // unrelated controls untouched
        g_fakeMode = 2;                   // frozen free camera: Alan has control
        CHECK(getAxis(1, 0) == 1.0f && getAxis(11, 0) == 11.0f);
        g_fakeMode = 6;
        CHECK(getAxis(1, 0) == 0.0f);
        g_fakeInputReady = nullptr;       // original's early-out path still works
        CHECK(getAxis(20, 0) == 0.0f);
        g_fakeInputReady = (void*)1;
        g_fakeMode = 0;
    }

    // isPressed detour: passes through (incl. its early-out) when no toggle is pending.
    {
        auto isPressed = (bool(__fastcall*)(int, int))(void*)(uint8_t* volatile)g_isPressed;
        CHECK(((volatile uint8_t*)g_isPressed)[0] == 0xFF);
        CHECK(isPressed(5, 0) == true && isPressed(6, 0) == false && isPressed(0x7F, 0) == false);
        g_fakePressReady = nullptr;
        CHECK(isPressed(5, 0) == false);  // early-out branch
        g_fakePressReady = (void*)1;
    }

    // Photo mode: queued presses are consumed one per query; state and speed.
    {
        auto isPressed = (bool(__fastcall*)(int, int))(void*)(uint8_t* volatile)g_isPressed;
        auto getPhoto = (PFN_AWT_GetPhoto)GetProcAddress(m, "AWT_GetPhoto");
        auto action = (PFN_AWT_PhotoAction)GetProcAddress(m, "AWT_PhotoAction");
        auto setTs = (PFN_AWT_SetTimeScale)GetProcAddress(m, "AWT_SetTimeScale");
        CHECK(getPhoto && action && setTs);
        AwtPhoto ph{};
        getPhoto(&ph);
        CHECK(ph.freeCamAvailable && ph.photoAvailable && ph.paused == 0 && ph.timeScale == 1.0f);
        CHECK(action(AWT_PHOTO_PAUSE) == 1);
        CHECK(isPressed(0x83, 0) == true && isPressed(0x83, 0) == false);  // exactly one press
        CHECK(action(AWT_PHOTO_STEP) == 0);                                // not paused: refused
        g_fakePaused = 1;
        getPhoto(&ph);
        CHECK(ph.paused == 1 && action(AWT_PHOTO_STEP) == 1 && isPressed(0x87, 0) && !isPressed(0x87, 0));
        g_fakePaused = 0;
        action(AWT_PHOTO_TIME_MINUS_HOUR);  // no TIME_BACKWARDFAST handler: 60 x TIME_BACKWARD
        int n = 0;
        while (isPressed(0x8B, 0) && n < 100) ++n;
        CHECK(n == 60);
        action(AWT_PHOTO_TIME_PLUS_HOUR);
        CHECK(isPressed(0x88, 0) && !isPressed(0x88, 0));
        action(AWT_PHOTO_FREECAM);
        CHECK(isPressed(0x7F, 0) && !isPressed(0x7F, 0));
        CHECK(isPressed(5, 0) == true);  // unrelated controls still pass through
        CHECK(setTs(0.25f) == 1 && g_fakeTs.step == 25 && g_fakeTs.scale == 0.25f);
        setTs(10.0f);
        CHECK(g_fakeTs.step == 400 && g_fakeTs.scale == 4.0f);  // clamped to 4x
        setTs(0.0f);
        CHECK(g_fakeTs.step == 1);  // clamped to 0.01x
        setTs(1.0f);
        getPhoto(&ph);
        CHECK(ph.timeScale == 1.0f && strstr(ph.keys, "Pause Pause") && strstr(ph.keys, "Speed NumpadMinus/Plus/Backspace"));
    }

    // Atlas patch: 0x1000/0x800 immediates doubled.
    int32_t aw, ah;
    memcpy(&aw, (const void*)((volatile uint8_t*)g_atlasCtor + 3), 4);
    memcpy(&ah, (const void*)((volatile uint8_t*)g_atlasCtor + 10), 4);
    CHECK(aw == 0x2000 && ah == 0x1000);

    // Allocator hook: the vtable slot now points into our DLL, and calls pass through.
    // Read through volatile: the compiler would otherwise fold the const vtable.
    void* slot1 = ((void* volatile const*)g_allocVtable)[1];
    CHECK(slot1 != (void*)g_allocate);
    FakeAllocator fa{g_allocVtable, {}, 8192, 4096, 1};
    auto alloc = (AllocateFn)slot1;
    int ok = 0;
    for (int i = 0; i < 2; ++i) ok += alloc(&fa, 2048, 2048, 0) == &fa;  // one call site
    CHECK(ok == 2);
    CHECK(alloc(&fa, 999, 512, 0) == nullptr);  // fake fails w == 999; flag 0 not scaled
    CHECK(alloc(&fa, 600, 512, 1) == &fa);      // light: scaled x2 to 1200x1024
    CHECK(alloc(&fa, 3000, 3000, 1) == &fa);    // light: x2 capped at 4096

    // Repack hook. Entries: x, y, w, h, flag (0x14 bytes); regions hold a handle.
    void* slot3 = ((void* volatile const*)g_allocVtable)[3];
    CHECK(slot3 != (void*)g_repack);
    auto repack = (void(__fastcall*)(void*))slot3;
    static uint8_t entries[16 * 0x14];
    static FakePacker packer{16384, 8192, {}, entries, 0};
    static FakeRegion regions[16];
    static uint8_t allocator[0x58 + 256 * 8];
    *(void**)(allocator + 0x30) = &packer;
    auto E = [](int i) { return (int*)(entries + i * 0x14); };
    auto frame = [&](int W, int H, std::initializer_list<std::pair<int, int>> sizes, int resizableFrom) {
        memset(entries, 0, sizeof(entries));
        memset(allocator + 0x58, 0, 256 * 8);
        packer.W = W, packer.H = H, packer.pad[0] = 0, packer.count = 0;
        for (auto [w, h] : sizes) {
            int i = (int)packer.count++;
            E(i)[0] = E(i)[1] = -7;  // garbage the packer must overwrite
            E(i)[2] = w, E(i)[3] = h, E(i)[4] = i >= resizableFrom;
            regions[i] = FakeRegion{{}, i, -1, -1, -1, -1};
            *(void**)(allocator + 0x58 + i * 8) = &regions[i];
        }
        repack(allocator);
    };
    auto pow2 = [](int v) { int s = 1; while (s < v) s <<= 1; return s; };
    auto validLayout = [&](int W, int H) {
        for (size_t i = 0; i < packer.count; ++i) {
            auto& r = regions[i];
            int s = pow2(r.w > r.h ? r.w : r.h);
            if (r.x < 0 || r.y < 0 || r.x + s > W || r.y + s > H) return false;
            if (r.x % s || r.y % s) return false;  // power-of-two aligned
            for (size_t j = 0; j < i; ++j) {
                auto& q = regions[j];
                int t = pow2(q.w > q.h ? q.w : q.h);
                if (r.x < q.x + t && q.x < r.x + s && r.y < q.y + t && q.y < r.y + s) return false;
            }
        }
        return true;
    };

    // 1) All slots <= 2048: the game's own packer runs (the fake halves entry 1).
    frame(16384, 8192, {{2000, 1900}, {1024, 1024}}, 1);
    CHECK(E(1)[2] == 512);
    CHECK(regions[0].x == -1);  // fake original doesn't copy placements

    // 2) Ultra at 4096: six ~4000 cascades + lights. Our packer places all, no overlap.
    frame(16384, 8192,
          {{4052, 4034}, {3528, 4014}, {2925, 3089}, {2002, 2025}, {1226, 1448}, {3700, 3600},
           {1024, 1024}, {1024, 1024}, {512, 512}, {512, 512}, {512, 512}},
          6);
    CHECK(packer.pad[0] == 1);
    CHECK(validLayout(16384, 8192));
    CHECK(regions[0].w == 4052 && regions[0].h == 4034 && regions[6].w == 1024);  // sizes preserved
    CHECK(regions[0].x == 0 && regions[0].y == 0);                                // largest at top-left

    // 3) Stock-size atlas can't hold 4096 cascades: lights shrink, a cascade fails.
    frame(8192, 4096, {{4000, 4000}, {4000, 4000}, {4000, 4000}, {1024, 1024}}, 3);
    CHECK(E(3)[2] < 1024);  // resizable light halved

    int e1w = 0;
    (void)e1w;
    CHECK(F(0)[0] == 0.3f);
    CHECK(objs[1].raw[0xB0] == 0);
    CHECK(F(2)[0] == 0.5f && F(2)[1] == 0.25f && F(2)[2] == 0.125f && F(2)[3] == 1.0f);
    CHECK(F(3)[0] == 5.0f);  // out of range, clamp off

    // Late registration (function-local static initialised later).
    uint8_t* e = Make(4, "HBAO:Radius In Meters", 1);
    F(4)[0] = 50; F(4)[1] = 0; F(4)[2] = 150;
    g_reg[4] = e;
    g_cnt = 5;
    Sleep(300);
    CHECK(F(4)[0] == 90.0f);  // active preset (matched case-insensitively) wins over [Tweaks]

    // Overlay API (what the ReShade add-on calls).
    {
        auto status = (PFN_AWT_GetStatus)GetProcAddress(m, "AWT_GetStatus");
        auto snapshot = (PFN_AWT_Snapshot)GetProcAddress(m, "AWT_Snapshot");
        auto set = (PFN_AWT_Set)GetProcAddress(m, "AWT_Set");
        auto reset = (PFN_AWT_Reset)GetProcAddress(m, "AWT_Reset");
        auto save = (PFN_AWT_SaveToIni)GetProcAddress(m, "AWT_SaveToIni");
        CHECK(status && snapshot && set && reset && save);
        AwtStatus st{};
        status(&st);
        CHECK(st.apiVersion == AWT_API_VERSION && st.registered == 5 && !strcmp(st.preset, "Dark Test"));
        static AwtInfo items[16];
        int n = snapshot(items, 16);
        CHECK(n == 5);
        auto find = [&](const char* name) -> AwtInfo* {
            for (int i = 0; i < n; ++i)
                if (!strcmp(items[i].name, name)) return &items[i];
            return nullptr;
        };
        AwtInfo* bias = find("HBAO:Bias");
        CHECK(bias && bias->type == AWT_FLOAT && bias->value[0] == 5.0f && bias->original[0] == 0.1f &&
              bias->min == 0.0f && bias->max == 1.0f && bias->source == AWT_SRC_TWEAKS);
        CHECK(find("HBAO:Radius In Meters")->source == AWT_SRC_PRESET);

        float v = 0.3f;
        CHECK(set("HBAO:Bias", &v) == 1);
        CHECK(F(3)[0] == 0.3f);  // immediate
        Sleep(150);
        CHECK(F(3)[0] == 0.3f);  // worker enforces the overlay value, not the ini's 5
        n = snapshot(items, 16);
        CHECK(find("HBAO:Bias")->source == AWT_SRC_OVERLAY);

        float on = 1.0f;  // Fog:Half Res Mode is false in [Tweaks]
        set("Fog:Half Res Mode", &on);
        CHECK(objs[1].raw[0xB0] == 1);
        CHECK(reset("Fog:Half Res Mode") == 1);
        Sleep(150);
        CHECK(objs[1].raw[0xB0] == 0);  // back to the ini value

        float r = 95.0f;  // set by the active preset -> saved into that preset's section
        set("HBAO:Radius In Meters", &r);
        status(&st);
        CHECK(st.overlayEdits == 2);
        CHECK(save() == 2);
        status(&st);
        CHECK(st.overlayEdits == 0);
        Sleep(300);  // worker reloads the rewritten ini
        CHECK(F(3)[0] == 0.3f && F(4)[0] == 95.0f);
        FILE* fi = _wfopen((dir + L"\\awtweaks.ini").c_str(), L"rb");
        static char ini[8192] = {};
        if (fi) fread(ini, 1, sizeof(ini) - 1, fi), fclose(fi);
        const char* tw = strstr(ini, "[Tweaks]");
        const char* pr = strstr(ini, "[Preset:Dark Test]");
        CHECK(tw && pr && strstr(tw, "HBAO:Bias=0.3\r\n") && strstr(tw, "HBAO:Bias=0.3\r\n") < strstr(ini, "[Preset:Other]"));
        CHECK(strstr(pr, "HBAO:Radius In Meters=95    ; preset beats [Tweaks]") != nullptr);
        CHECK(strstr(ini, "HBAO:Bias=5") == nullptr);  // replaced in place, not duplicated

        // v2: presets and the mod switch.
        auto getPresets = (PFN_AWT_GetPresets)GetProcAddress(m, "AWT_GetPresets");
        auto setPreset = (PFN_AWT_SetPresetActive)GetProcAddress(m, "AWT_SetPresetActive");
        auto setModOn = (PFN_AWT_SetModOn)GetProcAddress(m, "AWT_SetModOn");
        auto saveStartup = (PFN_AWT_SaveStartupPresets)GetProcAddress(m, "AWT_SaveStartupPresets");
        CHECK(getPresets && setPreset && setModOn && saveStartup);
        static AwtPreset ps[8];
        CHECK(getPresets(ps, 8) == 3);
        CHECK(!strcmp(ps[0].name, "Other") && !strcmp(ps[0].group, "Quality") &&
              !strcmp(ps[0].description, "Other one; with a semicolon") && !ps[0].active && ps[1].active &&
              !ps[2].active);
        CHECK(setPreset("other", 1) == 1);  // same group: replaces Dark Test
        Sleep(200);
        getPresets(ps, 8);
        CHECK(ps[0].active && !ps[1].active && F(4)[0] == 70.0f);
        CHECK(setPreset("Look1", 1) == 1);  // other group: combines
        Sleep(200);
        status(&st);
        CHECK(!strcmp(st.preset, "Other, Look1") && F(3)[0] == 0.2f);
        CHECK(setPreset("Nope", 1) == 0);
        setModOn(0);
        Sleep(200);
        status(&st);
        CHECK(st.modOff == 1 && F(4)[0] == 50.0f && F(3)[0] == 0.1f);  // everything back to stock
        setModOn(1);
        Sleep(200);
        CHECK(F(4)[0] == 70.0f && F(3)[0] == 0.2f);
        CHECK(saveStartup() == 1);
        Sleep(300);  // reload: Preset= now lists the same set
        status(&st);
        CHECK(!strcmp(st.preset, "Other, Look1"));
        fi = _wfopen((dir + L"\\awtweaks.ini").c_str(), L"rb");
        memset(ini, 0, sizeof(ini));
        if (fi) fread(ini, 1, sizeof(ini) - 1, fi), fclose(fi);
        CHECK(strstr(ini, "Preset=Other, Look1") != nullptr && strstr(ini, "PresetKey=0x7B") != nullptr);
        setPreset("Look1", 0);
        Sleep(200);
        CHECK(F(3)[0] == 0.3f);  // back to the [Tweaks] value saved from the overlay
    }

    // Preset benchmark: Stock + both Quality presets, frames counted in repack().
    {
        auto start = (PFN_AWT_StartBenchmark)GetProcAddress(m, "AWT_StartBenchmark");
        auto get = (PFN_AWT_GetBenchmark)GetProcAddress(m, "AWT_GetBenchmark");
        auto status = (PFN_AWT_GetStatus)GetProcAddress(m, "AWT_GetStatus");
        auto setPreset = (PFN_AWT_SetPresetActive)GetProcAddress(m, "AWT_SetPresetActive");
        CHECK(start && get);
        setPreset("Dark Test", 1);
        setPreset("Look1", 1);
        Sleep(100);
        AwtBenchmark b{};
        get(&b);
        CHECK(b.available && !b.running);
        CHECK(start() == 1);
        auto repack = (void(__fastcall*)(void*))((void* volatile const*)g_allocVtable)[3];
        static uint8_t entries2[0x14] = {};
        static FakePacker pk{16384, 8192, {}, entries2, 0};
        static uint8_t alloc2[0x58 + 256 * 8] = {};
        *(void**)(alloc2 + 0x30) = &pk;
        bool sawOther = false;
        ULONGLONG t0 = GetTickCount64();
        do {  // ~200 "fps": one repack per 5 ms
            repack(alloc2);
            Sleep(5);
            get(&b);
            if (b.running && !strcmp(b.current, "Other") && F(4)[0] == 70.0f) sawOther = true;
        } while ((b.running || b.resultCount == 0) && GetTickCount64() - t0 < 5000);
        CHECK(!b.running && b.resultCount == 3);
        CHECK(!strcmp(b.results[0].name, "Stock") && !strcmp(b.results[1].name, "Other") &&
              !strcmp(b.results[2].name, "Dark Test"));
        CHECK(b.results[0].fps > 50 && b.results[0].fps < 400 && b.results[2].fps > 50);
        CHECK(sawOther);  // presets really switched during the run
        Sleep(150);
        AwtStatus st{};
        status(&st);
        CHECK(!strcmp(st.preset, "Dark Test, Look1"));  // restored afterwards
        setPreset("Look1", 0);
        Sleep(100);
    }

    // Enforce: game resets a value.
    F(0)[0] = 0.6f;
    Sleep(300);
    CHECK(F(0)[0] == 0.3f);

    // Hot reload with clamping on.
    Sleep(50);
    WriteIni(dir + L"\\awtweaks.ini",
             "[Settings]\nClampToRange=true\nPollMs=20\n[Tweaks]\nHBAO:Bias=5\n"
             "Shadows:Ultra Cascade 4 Cull Radius=0.45\n");
    Sleep(800);
    CHECK(F(3)[0] == 1.0f);
    CHECK(F(0)[0] == 0.45f);
    // Removed from ini -> restored to engine originals.
    CHECK(objs[1].raw[0xB0] == 1);
    CHECK(F(2)[0] == 1.0f && F(2)[2] == 1.0f);
    CHECK(F(4)[0] == 50.0f);

    // XInput forwarding goes to the system DLL.
    using GetState = DWORD(WINAPI*)(DWORD, void*);
    auto gs = (GetState)GetProcAddress(m, MAKEINTRESOURCEA(2));
    uint8_t state[16] = {};
    DWORD r = gs ? gs(0, state) : 0xFFFFFFFF;
    CHECK(r == ERROR_SUCCESS || r == ERROR_DEVICE_NOT_CONNECTED);
    // Hidden ordinal (XInputGetStateEx) resolves through our own export walk.
    auto gsx = (GetState)GetProcAddress(m, MAKEINTRESOURCEA(100));
    DWORD rx = gsx ? gsx(0, state) : 0xFFFFFFFF;
    CHECK(rx == ERROR_SUCCESS || rx == ERROR_DEVICE_NOT_CONNECTED);

    // Cascade resolution guard: with the large-slot packer installed, at most 4096
    // (the game's own packer hangs on slots it can't place).
    {
        uint8_t* res = Make(5, "Shadows:Ultra High Resolution", 1);
        F(5)[0] = 2048; F(5)[1] = 0; F(5)[2] = 2048;
        g_reg[5] = res;
        g_cnt = 6;
        Sleep(100);
        auto set = (PFN_AWT_Set)GetProcAddress(m, "AWT_Set");
        auto reset = (PFN_AWT_Reset)GetProcAddress(m, "AWT_Reset");
        float want = 8192.0f;
        CHECK(set("Shadows:Ultra High Resolution", &want) == 1);
        CHECK(F(5)[0] == 4096.0f);
        Sleep(100);
        CHECK(F(5)[0] == 2048.0f);  // the ini at this point has ClampToRange=true (engine range 0..2048)
        want = 3072.0f;
        set("Shadows:Ultra High Resolution", &want);
        CHECK(F(5)[0] == 3072.0f);  // within the guard, applied immediately
        reset("Shadows:Ultra High Resolution");
        Sleep(100);
        CHECK(F(5)[0] == 2048.0f);
    }

    // List tweakable (type 5): engine strings at +0xB0 (0xA8 stride, text first),
    // count at +0xB8, selected index at +0xD0.
    {
        static const char* kOpts[] = {"Jitter8", "Poisson Disk", "Optimised PCF", "Spiral Gradient",
                                      "Vogel Disk Gradient"};
        static uint8_t items[5 * 0xA8] = {};
        for (int i = 0; i < 5; ++i) *(const char**)(items + i * 0xA8) = kOpts[i];
        uint8_t* lst = Make(6, "Shadow Filtering d3t:Function", 5);
        *(uint8_t**)(lst + 0xB0) = items;
        *(size_t*)(lst + 0xB8) = 5;
        *(int*)(lst + 0xD0) = 4;
        g_reg[6] = lst;
        g_cnt = 7;
        Sleep(100);
        auto snapshot = (PFN_AWT_Snapshot)GetProcAddress(m, "AWT_Snapshot");
        auto set = (PFN_AWT_Set)GetProcAddress(m, "AWT_Set");
        auto save = (PFN_AWT_SaveToIni)GetProcAddress(m, "AWT_SaveToIni");
        static AwtInfo snap[16];
        int n = snapshot(snap, 16);
        AwtInfo* li = nullptr;
        for (int i = 0; i < n; ++i)
            if (!strcmp(snap[i].name, "Shadow Filtering d3t:Function")) li = &snap[i];
        CHECK(li && li->type == AWT_LIST && li->value[0] == 4.0f && li->original[0] == 4.0f && li->max == 4.0f);
        CHECK(li && !strcmp(li->items, "Jitter8\nPoisson Disk\nOptimised PCF\nSpiral Gradient\nVogel Disk Gradient\n"));
        float pick = 1.0f;  // overlay picks "Poisson Disk"
        CHECK(set("Shadow Filtering d3t:Function", &pick) == 1 && *(int*)(lst + 0xD0) == 1);
        CHECK(save() == 1);
        Sleep(300);
        FILE* fi = _wfopen((dir + L"\\awtweaks.ini").c_str(), L"rb");
        static char ini[8192] = {};
        if (fi) fread(ini, 1, sizeof(ini) - 1, fi), fclose(fi);
        CHECK(strstr(ini, "Shadow Filtering d3t:Function=Poisson Disk") != nullptr);  // saved by name
        CHECK(*(int*)(lst + 0xD0) == 1);  // reloaded from the ini by name

        // By index, by name with other case, and a bad name (rejected, logged).
        WriteIni(dir + L"\\awtweaks.ini",
                 "[Settings]\nPollMs=20\n[Shadows]\nAtlasScale=2\n[Tweaks]\nShadow Filtering d3t:Function=2\n");
        Sleep(300);
        CHECK(*(int*)(lst + 0xD0) == 2);
        WriteIni(dir + L"\\awtweaks.ini",
                 "[Settings]\nPollMs=20\n[Shadows]\nAtlasScale=2\n[Tweaks]\nShadow Filtering d3t:Function=spiral gradient\n");
        Sleep(300);
        CHECK(*(int*)(lst + 0xD0) == 3);
        WriteIni(dir + L"\\awtweaks.ini",
                 "[Settings]\nPollMs=20\n[Shadows]\nAtlasScale=2\n[Tweaks]\nShadow Filtering d3t:Function=Nope\n");
        Sleep(300);
        CHECK(*(int*)(lst + 0xD0) == 4);  // invalid option -> stock option, not the previous one
        // Put back the ini the later dump checks expect.
        WriteIni(dir + L"\\awtweaks.ini",
                 "[Settings]\nClampToRange=true\nPollMs=20\n[Tweaks]\nHBAO:Bias=5\n"
                 "Shadows:Ultra Cascade 4 Cull Radius=0.45\n");
        Sleep(300);
        CHECK(*(int*)(lst + 0xD0) == 4);
    }

    Sleep(3500);  // let the dump fire
    FILE* f = _wfopen((dir + L"\\awtweaks_dump.txt").c_str(), L"rb");
    CHECK(f != nullptr);
    if (f) {
        char buf[4096] = {};
        fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        CHECK(strstr(buf, "HBAO:Radius In Meters=50    ; float, range 0..150\n") != nullptr);
        CHECK(strstr(buf, "HBAO:Bias=0.1    ; float, range 0..1, now 1\n") != nullptr);
        CHECK(strstr(buf, "Fog:Half Res Mode=true\n") != nullptr);
    }
    f = _wfopen((dir + L"\\awtweaks.log").c_str(), L"rb");
    if (f) {
        static char log[65536] = {};
        fread(log, 1, sizeof(log) - 1, f);
        fclose(f);
        CHECK(strstr(log, "Shadow atlas 8192x4096 large=1") != nullptr);
        CHECK(strstr(log, "flag=0 calls=2 failed=0 w=2048..2048 h=2048..2048") != nullptr);
        CHECK(strstr(log, "flag=0 calls=1 failed=1 w=999..999 h=512..512") != nullptr);
        CHECK(strstr(log, "flag=1 calls=1 failed=0 w=1200..1200 h=1024..1024") != nullptr);
        CHECK(strstr(log, "flag=1 calls=1 failed=0 w=4096..4096 h=4096..4096") != nullptr);
        CHECK(strstr(log, "LightShadowScale 2 -> 1") != nullptr);  // live reload
        CHECK(strstr(log, "WARNING: Shadows:Ultra High Resolution=8192 capped to 4096") != nullptr);
        CHECK(strstr(log, "Shadow Filtering d3t:Function: Poisson Disk -> Optimised PCF") != nullptr);
        CHECK(strstr(log, "Spiral Gradient | Vogel Disk Gradient); using stock Vogel Disk Gradient") != nullptr);
        CHECK(strstr(log, "already = Nope") == nullptr);
        CHECK(strstr(log, "3 presets, toggle=F11 preset key=F12") != nullptr);
        CHECK(strstr(log, "HBAO:Radius In Meters: 50 -> 90") != nullptr);
        // Frame 1's fake original copies no placements (-1s => bad); frame 3 overflows.
        CHECK(strstr(log, "maxEntries=11 maxLive=11") != nullptr);  // frame count varies with the benchmark test
        CHECK(strstr(log, "overflowFrames=1 halvedEntries=2 badPlacements=2") != nullptr);
        CHECK(strstr(log, "large-slot packer total=2 failed=1") != nullptr);
        CHECK(strstr(log, "Large-slot packer active") != nullptr);
        CHECK(strstr(log, "WARNING: large-slot packer could not place 1 frame(s)") != nullptr);
    }
    printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    fflush(stdout);
    ExitProcess(fails ? 1 : 0);
}
