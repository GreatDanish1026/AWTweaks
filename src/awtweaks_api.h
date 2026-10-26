// AWTweaks overlay API: exported by xinput1_4.dll (the main mod), used by the
// ReShade add-on. Plain C structs; everything is copied, so no lifetimes to manage.
#pragma once
#include <cstdint>

#define AWT_API_VERSION 9

enum AwtType : int32_t { AWT_BOOL = 0, AWT_FLOAT = 1, AWT_COLOR = 3, AWT_LIST = 5 };  // others: read-only
enum AwtSource : int32_t { AWT_SRC_NONE = 0, AWT_SRC_TWEAKS = 1, AWT_SRC_PRESET = 2, AWT_SRC_OVERLAY = 3 };

struct AwtInfo {
    char name[96];
    int32_t type;        // AwtType, or another engine type (not editable)
    int32_t source;      // AwtSource: who currently overrides it
    float value[4];      // current value (bool: value[0] is 0/1; float: value[0])
    float original[4];   // value when the engine registered it
    float min, max;      // engine slider range (floats); list: 0 .. option count - 1
    char items[192];     // list options, each followed by a newline (lists only)
};

struct AwtStatus {
    int32_t apiVersion;
    int32_t modOff;          // A/B toggle (F10) has everything at stock
    int32_t overlayEdits;    // unsaved overlay overrides
    int32_t registered;      // tweakables registered so far
    char preset[64];         // active presets, comma-separated, "" = none
    // v6: AWTweaks shadow shader settings (set them with AWT_Set("AWTweaks:Sun Size") /
    // ("AWTweaks:Contact Shadows"); they take effect with the add-on's enhanced shader).
    float sunSize;           // degrees, 0 = the game's filter
    float contactShadows;    // metres, 0 = off
    int32_t shadowFxSource;  // AwtSource of whichever of the two is set most specifically
    // v7: AWTweaks AO shader setting (AWT_Set("AWTweaks:GTAO"): strength, 0 = the game's
    // HBAO+ .. 3; takes effect with the add-on's [Shaders] AOShader=Enhanced).
    float gtao;
    int32_t aoFxSource;
    // v8: AWTweaks spotlight shader settings (AWT_Set("AWTweaks:Flashlight Softness") /
    // ("AWTweaks:Flashlight Contact Shadows"); take effect with [Shaders] SpotShader=Enhanced).
    float flashSoftness;     // lens radius, cm; 0 = the game's filter
    float flashContact;      // metres, 0 = off
    int32_t flashFxSource;
    // v9: AWTweaks spotlight beam shader setting (AWT_Set("AWTweaks:Beam Quality"): 0 = the
    // game's march .. 3; takes effect with [Shaders] VolumetricShader=Enhanced).
    float beamQuality;
    int32_t beamFxSource;
};

// v3: photo mode.
struct AwtPhoto {
    int32_t freeCamAvailable;  // [FreeCamera] hooks installed
    int32_t photoAvailable;    // [PhotoMode] globals found
    int32_t cameraMode;        // 0 player, 1 free camera (flying), 2 frozen (Alan playable), other = game camera
    int32_t paused;
    float timeScale;           // 1.0 = normal speed
    char keys[160];            // human-readable key summary for the UI
};

enum AwtPhotoAction : int32_t {
    AWT_PHOTO_FREECAM = 0,     // cycle: player -> fly -> frozen -> player
    AWT_PHOTO_PAUSE = 1,       // toggle
    AWT_PHOTO_STEP = 2,        // one frame (while paused)
    AWT_PHOTO_TIME_MINUS_HOUR = 3,
    AWT_PHOTO_TIME_MINUS_MINUTE = 4,
    AWT_PHOTO_TIME_PLUS_MINUTE = 5,
    AWT_PHOTO_TIME_PLUS_HOUR = 6,
};

// v4: preset benchmark.
struct AwtBenchmark {
    int32_t available;   // needs AtlasScale=2 (frames are counted in the shadow packer hook)
    int32_t running;
    int32_t stage, stages;
    char current[64];    // preset being measured ("Stock" = no Quality preset)
    int32_t resultCount;
    struct {
        char name[64];
        float fps;       // average over 5 s after a 2 s settle
    } results[8];
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
// Overlay override: bool uses v[0] != 0, float v[0], color v[0..3], list v[0] = option index.
// Applies at once.
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
// v3: photo mode.
typedef int(__cdecl* PFN_AWT_GetPhoto)(AwtPhoto* out);
typedef int(__cdecl* PFN_AWT_PhotoAction)(int action);   // AwtPhotoAction
typedef int(__cdecl* PFN_AWT_SetTimeScale)(float scale);  // 0.01 .. 4
// v4: runs Stock + each Quality preset, 7 s each, then restores the presets.
typedef int(__cdecl* PFN_AWT_StartBenchmark)(void);
typedef int(__cdecl* PFN_AWT_GetBenchmark)(AwtBenchmark* out);
}
