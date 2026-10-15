// AWTweaks overlay API: exported by xinput1_4.dll (the main mod), used by the
// ReShade add-on. Plain C structs; everything is copied, so no lifetimes to manage.
#pragma once
#include <cstdint>

#define AWT_API_VERSION 2

enum AwtType : int32_t { AWT_BOOL = 0, AWT_FLOAT = 1, AWT_COLOR = 3 };  // others: read-only
enum AwtSource : int32_t { AWT_SRC_NONE = 0, AWT_SRC_TWEAKS = 1, AWT_SRC_PRESET = 2, AWT_SRC_OVERLAY = 3 };

struct AwtInfo {
    char name[96];
    int32_t type;        // AwtType, or another engine type (not editable)
    int32_t source;      // AwtSource: who currently overrides it
    float value[4];      // current value (bool: value[0] is 0/1; float: value[0])
    float original[4];   // value when the engine registered it
    float min, max;      // engine slider range (floats only)
};

struct AwtStatus {
    int32_t apiVersion;
    int32_t modOff;          // A/B toggle (F10) has everything at stock
    int32_t overlayEdits;    // unsaved overlay overrides
    int32_t registered;      // tweakables registered so far
    char preset[64];         // active presets, comma-separated, "" = none
};

struct AwtPreset {
    char name[64];
    char group[32];          // e.g. "Quality", "Look"; "" = combinable with anything
    char description[256];
    int32_t active;
};

extern "C" {
typedef int(__cdecl* PFN_AWT_GetStatus)(AwtStatus* out);
// Copies up to `cap` entries; returns the total count.
typedef int(__cdecl* PFN_AWT_Snapshot)(AwtInfo* out, int cap);
// Overlay override: bool uses v[0] != 0, float v[0], color v[0..3]. Applies at once.
typedef int(__cdecl* PFN_AWT_Set)(const char* name, const float* v);
// Drops the overlay override for `name` (nullptr = all); ini/preset/stock value returns.
typedef int(__cdecl* PFN_AWT_Reset)(const char* name);
// Writes overlay overrides into awtweaks.ini (into the active preset's section for
// names that preset sets, else [Tweaks]); returns how many were saved, -1 on error.
typedef int(__cdecl* PFN_AWT_SaveToIni)(void);
// v2: presets and the mod switch.
// Copies up to `cap` [Preset:...] definitions in ini order; returns the total count.
typedef int(__cdecl* PFN_AWT_GetPresets)(AwtPreset* out, int cap);
// Turns a preset on/off. Turning one on turns off others in its group (and the mod on).
typedef int(__cdecl* PFN_AWT_SetPresetActive)(const char* name, int active);
// The A/B switch: 0 = everything at stock.
typedef int(__cdecl* PFN_AWT_SetModOn)(int on);
// Saves the active presets as the ini's startup Preset= line.
typedef int(__cdecl* PFN_AWT_SaveStartupPresets)(void);
}
