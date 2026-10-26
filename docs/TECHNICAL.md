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
debug variants. The originals were built with a 2019 DXC; two `precise`
products and one exact constant keep the new compiler's output bit-identical.

The shipped build (`ENHANCED`) adds two effects on top, off unless AWTweaks
sends settings, in which case it runs the original code exactly:
- **Soft shadows (PCSS).** A 16-tap blocker search over up to 40 atlas texels
  averages the occluder depth; the penumbra is (receiver − blocker depth) ×
  tan(half sun angle), converted with the shadow projection's own scales
  (`|row 0|` UV per metre, `|row 2|` depth per metre), clamped between the
  stock radius and 40 texels. A 24-tap Vogel PCF with a distance-scaled bias
  (45° slopes stay clean) filters it.
- **Contact shadows.** A 12-step screen-space march towards the light,
  `normalize(cross(row 0, row 1))` signed so shadow depth decreases. A view
  point maps back to a pixel through the view ray's per-pixel derivatives
  (`ddx/ddy(TEXCOORD0)`), and scene depth is linear depth × `viewRay.z ×
  g_fInvNear`. Note the linear depth buffer is not 0..1 (it holds roughly
  metres). It fades out between 25 and 50 m.

**Settings channel.** The engine uploads only the constants the game's shader
reads (writing "PCF Samples", read by no shader, never reached the GPU). The
DLL turns the pseudo-tweakables `AWTweaks:Sun Size` / `AWTweaks:Contact
Shadows` into the low digits of `Cascade Filter Size`, which only ShadowBuffer
reads: size rounded to 0.1 + 0.00001 × (1 + sun in 0.1° × 100 + contact in
2 cm). That moves the stock radius by under 0.4%. Any size someone sets is kept
on the 0.1 grid so it can't be misread. F10 restores the plain value, so the
shader runs stock. API v6 reports the effective values for the overlay
sliders.

`ShadowShader=Debug` shows stripes (32 px, or 128 px while settings arrive). With
contact shadows on it instead splits the screen in three: contact shadow alone,
screen-mapping self-check, and why each march ended.

Measured at 4K (paused, same view): Stock 147.5, Ultra+ 128.0 (0.5°, 0.3 m),
Cinematic 120.1 FPS (1.0°, 0.6 m).

The add-on's `create_pipeline` handler (`src/addon/shader_replace.cpp`) matches
a pipeline's pixel shader by DXBC digest (bytes 4–20) and size and swaps in the
rebuild; `init_pipeline` logs any game shader that slipped through.
`ShadowShader=Enhanced | Original | Debug` (`Rebuilt` is an alias for
Enhanced), read at game start. In game (Ultra) only the 12-tap fade-in
variant is created.

`test\shader_compare.cpp` renders an original blob and a rebuild on D3D12 WARP
with the same synthetic depth, shadow map and constants and compares every
pixel. With settings off all four match bit for bit. Given a settings code
(`1 + sun × 100 + contact`) as a fifth argument, it instead checks the
enhanced output for NaNs and out-of-range values and writes both shadow masks
as PGM:
```
dxc -T vs_6_0 -E main test\fullscreen_vs.hlsl -Fo bin\shaders\test_vs.dxil
shader_compare.exe bin\shaders\test_vs.dxil <original_ps.dxil> bin\shaders\Sb12Start.dxil 10
shader_compare.exe bin\shaders\test_vs.dxil <original_ps.dxil> bin\shaders\Sb12Start.dxil 3 1031
```
The original blobs come from the game's `ShadowBuffer.shaderbin` and aren't in
the repository.

### Ambient occlusion shader replacement (`[Shaders] AOShader`)
`HBAO.shaderbin` is NVIDIA HBAO+: deinterleave depth into 16 quarter-res
layers, reconstruct normals, `coarseAOPS` per layer, reinterleave, blur, copy.
`coarseAOPS` comes in 4, 6 and 8 directions (4 steps each; the first step of
each direction is the small-scale term, the rest large-scale), with the
background/foreground radius clamps and the depth-threshold fade. Output
x = AO, y = view depth for the blur. In game (Ultra) only the 6-direction
variant is created.

`src/shaders/hbao_coarse.hlsl` rebuilds all three (`NUM_DIRECTIONS`), plus a
`DEBUG_STRIPES` pattern (full occlusion in 32-px stripes: 8 px per layer, so
they survive reinterleaving). `shader_compare --hbao` feeds RGBA normals,
quarter-res view depth, two plain samplers and HBAO+ constants: all three
match the originals bit for bit. The add-on's replacement code handles both
shader families, each with its own `[Shaders]` key and stats.

**GTAO (`AOShader=Enhanced`).** With `AWTweaks:GTAO` above 0 the pass runs
ground-truth AO (Jimenez et al. 2016) in place of the HBAO+ loop. It uses the
same quarter-res layers, distance falloff, per-pass jitter and depth fade, but
half of HBAO+'s radius (`kGtaoRadiusScale`). The game's radius is huge
(`HBAO:Radius In Meters` 50, held back only by HBAO+'s screen-space clamps), and
over all of it GTAO's highest horizon spread crease shading into wide smudges
around clutter like coats and shelves. Per pixel it searches 4 slices both
ways, 6 steps per side, for the highest horizon. It then integrates
cosine-weighted visibility between the two horizons analytically, against the
normal projected into the slice. Each slice is divided by its open-hemisphere
integral, so a flat surface is exactly 1. Thickness heuristic: a sample below
the horizon pulls it down by 15 % × its distance falloff, so samples beyond the
radius can't erase a nearby occluder. Occlusion = (1 − visibility) × strength
× 2 × (SmallScaleAO + LargeScaleAO) / 2. The output still goes through the
game's blur and power exponent.

Settings channel: the DLL rounds `HBAO:Bias` (read only by `coarseAOPS`) to
0.001 and adds 0.000001 × strength in 0.1 steps (1–30); F10 or strength 0
leaves the plain value and the original code runs. API v7 reports the
effective strength. `AOShader=Debug` shows 32-px stripes, or 128 px while GTAO
is on.

`shader_compare --hbao [--flat | --blocks | --corner | --halo] ... <seeds> <code>`
checks the GTAO output. A flat wall comes out 1.000. In a wall corner, strength 1
is a little darker than HBAO+ right in the crease (0.69 vs 0.76) but about half
as wide (0.35–0.8× its total occlusion). `--halo`, a square 0.6 m in front of a
wall with a 4–9 m radius, leaves the wall clean. In game the cost is too small
to measure: Stock 156.0, Ultra+ 135.9 (GTAO 1.0), Cinematic 129.8 FPS (GTAO
1.5), at a different spot from the shadow measurements.

### Flashlight shadows (`[Shaders] SpotShader`)
Shadow-casting spotlights (Alan's flashlight, some lamps) are a deferred pass,
`DeferredLight.shaderbin` "spotlightPS". `[Debug] WatchShaders` showed only one
of its variants is ever created: the one with a shadow map and projection
texture (blob #25, 11615 bytes, 1–4 draws a frame). Material shaders bind a
`g_sShadowMap` too, but don't draw it.

The shader:
- reads the G-buffer (normals, tangent and gloss, linear depth = view z);
- multiplies falloff `saturate(exp(−d·f.x) − f.y)` × colour × projection
  texture;
- shadows with a 10-tap Vogel PCF of radius `g_fPCFSize_d3t` ("Shadow Filtering
  d3t:Filter Size") × 0.5 texels, rotated by interleaved gradient noise and
  blended by `g_fShadowLOD`;
- lights with a Ward BRDF where there's a tangent (αx = gloss × 255, αy = 0.2),
  otherwise gloss-driven Blinn-Phong;
- writes diffuse rgb and specular luminance × exposure, clamped to 32.

`src/shaders/deferred_spotlight.hlsl` rebuilds it bit for bit. That took the
game's float constants via `asfloat`, `pow(x, 2)` written as
`exp2(log2(|x|) × 2)` as the original compiler emitted it, and `precise` on
the colour chain, the diffuse term and the final exposure multiply to keep the
original's operation order. `shader_compare --spot` feeds the six textures and
spotlight constants (a flashlight beside the camera, perspective light and
shadow matrices): 100% of pixels match on 10 seeds, covering Ward and Blinn
paths, zero gloss and partial shadow fades.

Enhanced (with the settings channel below):
- **Soft shadows (PCSS).** It searches 16 taps for blockers, then widens a
  24-tap filter to lens × (receiver − blocker) / blocker, the penumbra a disc
  light of that radius casts. Shadow depth is perspective (z/w with
  z = a·w + b along the light's rays); `a` and `b` are measured per pixel from
  the receiver and a point twice as far from the light, then blocker depth is
  converted back to metres. Taps get a slope bias `|b| / w² / (UV per metre)`
  per UV unit of offset.
- **Contact shadows.** A 12-step screen-space march towards the light's
  position, mapped to pixels through the view ray's derivatives like the sun's.

Settings channel: `AWTweaks:Flashlight Softness` (lens radius, 0.2 cm steps)
and `AWTweaks:Flashlight Contact Shadows` (2 cm steps) ride in Filter Size,
rounded to 0.1, + 0.00001 × (1 + lens × 100 + contact). The material shaders
that also read it don't notice the extra digits. API v8 reports both.
`SpotShader=Debug` blanks the spotlights' light in 32-px stripes, 128 px while
the settings arrive.

In game, softness 3 cm turns the stair-stepped edge of a door frame's shadow
into a smooth penumbra. Contact shadows add little: a line where frames meet
walls, and they dim a highlight at a depth edge. So the presets use softness
2 cm (Ultra+) and 3 cm (Cinematic) and leave contact off. Night benchmark with
the flashlight on (presets as a whole): Stock 155.8, Ultra+ 150.5, Cinematic
143.5 FPS.

### Skin and faces
Characters' skin is `Skin_d3t.shaderbin` (the older `Skin.shaderbin` is never
drawn on PC). Three passes per character:
- G-buffer: normal and specular maps, to 3 targets.
- Depth/shadow.
- Composition (`material`): it combines the deferred light buffer with the sun
  (shadow mask and projection map), the character light rig, flashlight bounce,
  rim light, 8 ambient lights and 20 wrinkle maps. Its second target carries
  specular, with view depth in `.w`.

`Eye.shaderbin` never draws.

`SSSSS.shaderbin` "sss_blurPS" is Jimenez's separable subsurface scattering
(2012). It runs a 17-tap blur twice, horizontal then vertical, on pixels whose
specular `.w` is above 0, with a red-heavy skin profile. Step =
`g_fSSSWidth / (tan(fov/2) × depth)`, so the width is in metres. Each tap is
pulled back to the centre colour by
`saturate(g_fFollowSurfaceScale × width / tan(fov/2) × |Δdepth|)`; the
reference code's constant there is 300. The engine ships `Skin Shader:SSS Width`
0.0002 (0.2 mm, about one pixel) and `Follow Surface Scale` 30000, which cancels
the blur wherever two taps differ by about 2 mm in depth. Together the effect
is practically off. The presets use 0.008 / 300. In game the change follows
features (eyelids, nostrils, lips, mostly red) and stays subtle under the
game's desaturated grade. The pass already ran, so there's no added cost.

The character light rig is three invisible lights per character: front-left,
front-right and behind, at 0.31 / 0.31 / 0.05 at night. With equal lights 1 and 2,
a face is lit evenly from both sides, so the presets use 0.5 / 0.1 at night.
`HBAO:Skin Intensity` and `Eye Intensity` showed no effect from 0 to 1.

Diagnostics:
- `[Shaders] SSSShader=Debug` replaces the blur with
  `src/shaders/sss_debug.hlsl`: on skin, green stripes show where the blur runs
  and red stripes scale with `g_fSSSWidth`. It proved the mask covers the face
  and the width slider reaches the shader.
- `[Debug] WatchShaders=SSSSS,Skin_d3t,...` (`src/addon/shader_watch.cpp`)
  reads those shaderbins and logs which of their pixel shaders get pipelines and
  how many draws use each every 5 s, numbered like the extracted blobs.
- Advanced sliders whose stock value is under 1% of their range, like SSS Width
  in 0..1, are logarithmic so they can be dragged.

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
restore-on-removal, XInput forwarding, the dump and the shadow-, flashlight-
and AO-settings packing.
