# AWTweaks — technical notes

Overrides Remedy's internal tuning variables ("tweakables") — shadow cascades,
shadow-map sizes, HBAO, volumetric light, fog and ~1,000 more — from an INI file,
with live reload while the game runs.

## Install
Copy `bin\xinput1_4.dll` and `awtweaks.ini` next to `Game_f_x64_EOS.exe`.
Uninstall by deleting `xinput1_4.dll`.

The game imports `XINPUT1_4.dll`, so the proxy loads automatically; all XInput
calls are forwarded to `System32\xinput1_4.dll`. `dxgi.dll` is left free for ReShade.

## Files written next to the game
| File | What |
|---|---|
| `awtweaks.log` | What was found and applied, each run |
| `awtweaks_dump.txt` | Every registered tweakable with its **original** value and range (`now X` marks overridden ones). Copy lines from here into the INI. |

Tweakables register lazily (when their code first runs), so the dump grows as
you load levels. Some values (e.g. shadow map sizes) are only read on allocation:
switch the in-game quality preset or restart to see them take effect.

## How it works
Remedy's tweakable constructor appends each object to a global array
(`Tweakable* [0x500]` + count). The DLL finds that array via a byte signature
in the constructor, then polls it from a background thread:

| Offset | Field |
|---|---|
| `+0x00` | `const char*` name |
| `+0xA8` | type: 0 bool, 1 float, 3 color (float4); others unsupported |
| `+0xB0` | value (float: value, min `+0xB4`, max `+0xB8`) |

### Shadow maps
All sun cascades and spot/point-light shadows share one atlas, packed in
power-of-two slots by `rend::ShadowMapAllocator`. Stock: 8192x4096. On Ultra the
sun uses 6 cascades per frame, each fitted to ≤ `Shadows:Ultra High Resolution`
(2048), plus 512²/1024² light maps — the atlas is full.

`[Shadows] AtlasScale=2` patches the allocator ctor's size immediates before
the renderer starts (16384x8192, D3D12's max; colour + depth targets are created
from the same fields). With `Shadows:Ultra High Resolution=4096` the cascades
request up to ~4050² (4x the texels).

The engine's packer walks the atlas in fixed 2048x2048 cells and never places a
region whose power-of-two side is larger — it (and everything queued behind it)
is left at (0,0), so cascades overlapped and flickered. With `AtlasScale=2` the
DLL replaces `repack()` (allocator vtable slot 3) on frames that need a >2048
slot: same overflow rule (halve resizable light regions), then a quadtree of
power-of-two squares, largest first. Measured in game at 4096: every frame
packed, 0 failures, atlas ≤ 77% full, no FPS change.

`[Shadows] LightShadowScale=2` multiplies spot/point-light shadow requests
(per-light authored size, doubled on Very High/Ultra: 512/1024 → 1024/2048, cap
4096) in the `allocate()` hook. Lights are the only `flag=1` (resizable) requests,
which the engine already halves on overflow, so the renderer takes their size
from the packed region. Measured in game: atlas ≤ 80% full, no overflow.

`[Debug] LogShadowAllocs=true` logs every request by size and caller, plus
per-interval repack stats (fill, overflow, halvings, bad placements).

### Free camera (`[FreeCamera]`, F7)
The developer free camera ships intact (camera manager modes: 0 player, 1/2/6
FreeCamera; the per-frame toggle cycles 0 → 1 fly → 2 frozen/Alan playable → 0),
but nothing on PC fires its `TOGGLE_FREECAMERA` (127) control — binding it in
`keybinds.xml` is accepted yet never triggers — and its analog controls 9..16
(`FREECAMERA_X/Z/HEADING/PITCH/UP/DOWN/FASTER/SLOWER`) have no PC binding.
Two inline detours (installed from DllMain, 14-byte abs jump + trampoline that
re-encodes the RIP-relative prologue):

- `isPressed(int)`: the hotkey makes the next query for 127 return true once,
  so the game switches cameras itself on its own thread.
- `getAxis(int)`: in mode 1/6, feeds 9/10 from `MOVEMENT_X/Y` (0/1), 11/12 from
  `CAMERA_X/Y` (5/6, mouse/right stick), 13..16 from Space/LCtrl/LShift/LAlt,
  and returns 0 for the player's controls 0..8 so Alan stays put.

`LookSensitivity` / `InvertPitch` apply live; `[Debug] LogFreeCamInput` logs
per-control call counts and peaks every 2 s.

### In-game sliders (ReShade add-on)
`bin\AWTweaks.addon64` is a ReShade add-on (needs ReShade 6.8+ *with full add-on
support*). Copy it next to the game; open ReShade (Home) → **AWTweaks** tab:

- **Top (everyday):** Mod enabled switch, **Quality** presets as radio buttons
  (Stock / Ultra+ / Cinematic), **Look** presets as checkboxes (Darker Nights),
  each with its description, and *Use these presets at startup* (rewrites
  `Preset=`).
- **Advanced (collapsed):** every registered tweakable grouped by category, with
  sliders / checkboxes / colour pickers, a filter, "Only changed", per-item undo,
  and **Save to awtweaks.ini** (written in place into `[Tweaks]`, or into the
  preset section that set the value). Hover a row for its stock value.

Presets: `[Preset:Name]` sections with optional `Group=` (one active preset per
group) and `Description=`. `Preset=` takes a comma list; F9 cycles the Quality
group.

| | Stock | Ultra+ | Cinematic |
|---|---|---|---|
| Sun cascade resolution | 2048 | 3072 | 4096 |
| Every object casts sun shadows | – | – | yes |
| HBAO small-scale / exponent | 1.1 / 2.1 | 1.4 / 2.4 | 1.4 / 2.4 |
| Fog and rain resolution | half | full | full |
| Light-shaft blur / intensity | 1 / 0.55 | 2 / 0.55 | 2 / 0.7 |
| Small foliage draw distance | 60 m | 90 m | 240 m (cutscene value) |
| `LOD:LOD detail` (switch distance × size) | 0.3 | 0.5 | 1.0 |
| High-quality skinning range | 16 m | 20 m | 20 m |

**Measure FPS of each** (overlay, Quality group) runs Stock and every Quality
preset for 2 s settle + 5 s measure, counting rendered frames in the repack
hook, then restores the user's presets. It only shows a cost when the frame
rate isn't capped (V-Sync / Reflex / driver limiter).

The ImGui calls go through ReShade's function table, which is MSVC-built:
`GetContentRegionAvail` returns `ImVec2` via a hidden pointer there, so the
add-on (GNU ABI) calls that entry with the MSVC convention (`AvailWidth()`).

Overlay edits are overrides held by the main DLL: they beat `[Tweaks]` and
presets until saved or undone, and F10 still flips everything to stock. The
add-on talks to `xinput1_4.dll` through the C API in `src/awtweaks_api.h`
(`AWT_GetStatus/Snapshot/Set/Reset/SaveToIni`). Headers for ReShade 6.8.0 and
Dear ImGui 1.92.5 (the version that ReShade's overlay table requires) are in
`third_party/` with their licences.

### Photo mode (`[PhotoMode]`)
The game still polls its developer controls every frame; the `isPressed`
detour now queues "presses" per control id, each query consuming one:

| Key (default) | Control | Effect |
|---|---|---|
| F7 | `TOGGLE_FREECAMERA` 0x7F | player → fly → frozen (Alan playable) → player |
| F8 | `PAUSE1` 0x83 | toggle pause (`ModeSwitch.cpp`) |
| F6 | `JUMP_ONE_FRAME` 0x87 | one frame, only while paused |
| PageUp / PageDown | `TIME_FORWARD` 0x8A / `TIME_BACKWARD` 0x8B | ±60 s game clock; Shift: `TIME_FORWARDFAST` 0x88 (+3600 s) or 60× backward |
| Minus / Plus / Backspace | — | game speed 0.01–4× by writing the time-scale globals directly (int step ÷100 + float) |

The time-scale step and the paused flag are found by signature
(`TIMESCALE_DOWN` handler, `JUMP_ONE_FRAME` site). The overlay's Photo mode
section has the same controls plus a speed slider.

### Shadow shader replacement (`[Shaders]`, needs the add-on)
Every material samples `g_sShadowBuffer`, a screen-space sun/moon shadow mask
written only by `ShadowBuffer.shaderbin` (`update_shadow_buffer_PS`). Its four
pixel-shader variants are a 12- or 10-tap Vogel-disk PCF, rotated per pixel by
interleaved gradient noise, fading in at the cascade's start or out towards its
end; output is (shadow, view depth packed into two channels, cascade blend).
They read only `Cascade Filter Size`, the bias and a per-cascade CPU-side
`g_fPCFSize` — the `PCF Samples`, `Filter Size` and `Function` tweakables never
reach them.

`src/shaders/shadow_buffer.hlsl` rebuilds all four from the game's DXIL (same
bindings, `$Globals` layout and signatures, so it fits the game's root
signature). `build_shaders.ps1` compiles them with the Windows SDK's DXC
(signed via `dxil.dll`) into the committed `shadow_buffer_blobs.h`, plus
`DEBUG_STRIPES` variants. The originals were built with a 2019 DXC; two
`precise` products and one exact constant keep the new compiler's output
bit-identical.

The add-on's `create_pipeline` handler (`src/addon/shader_replace.cpp`) matches
a pipeline's pixel shader by DXBC digest (bytes 4–20) and size and swaps in the
rebuild; `init_pipeline` logs any game shader that slipped through.
`ShadowShader=Original | Rebuilt | Debug`, read at game start. In game (Ultra)
only the 12-tap fade-in variant is created.

`test\shader_compare.cpp` renders an original blob and a rebuild on D3D12 WARP
with the same synthetic depth, shadow map and constants and compares every
pixel; all four match bit for bit:
```
dxc -T vs_6_0 -E main test\fullscreen_vs.hlsl -Fo bin\shaders\test_vs.dxil
shader_compare.exe bin\shaders\test_vs.dxil <original_ps.dxil> bin\shaders\Sb12Start.dxil 10
```
The original blobs come from the game's `ShadowBuffer.shaderbin` and aren't in
the repository.

### Hotkeys and presets
`ToggleKey` (F10) A/B-toggles every override and `LightShadowScale` back to
stock; `PresetKey` (F9) cycles `[Preset:Name]` sections, which apply on top of
`[Tweaks]` (`Preset=` picks the startup one).

Verified against build 34885 (`.keystoneBuild`). If a patch changes the
constructor the log says "signature not found" and the mod stays idle.

## Build
```powershell
winget install MartinStorsjo.LLVM-MinGW.UCRT
.\build.ps1 -Install "E:\Non-Steam Games\AlanWakeRemastered"
```

## Test
`test\harness.cpp` fakes the registry (same signature and layout) and checks
float/bool/color writes, late registration, enforce, hot reload, clamping,
restore-on-removal, XInput forwarding and the dump.
