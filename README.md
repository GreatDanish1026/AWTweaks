# AWTweaks 1.2.0

Graphics, lighting and photo-mode mod for **Alan Wake Remastered** (PC, Epic
Games Store version only; not the original Alan Wake on Steam).

- **Sharper, fuller shadows** – sun and moon shadow cascades up to 4096
  (4× the detail of Ultra), every object casting shadows in the distance,
  2× sharper flashlight and lamp shadows.
- **Soft and contact shadows** (with the ReShade add-on) – sun and moon shadows
  soften the farther they fall from what casts them, and small contact shadows
  ground objects where they touch.
- **Soft flashlight shadows** (with the ReShade add-on) – shadows from Alan's
  flashlight and shadow-casting lamps are sharp where objects touch and soften
  with distance, as from a real lens, instead of hard stair-stepped edges.
- **Steadier flashlight beams** (with the ReShade add-on) – light beams in the
  air get filtered shadows and more steps where they cross shadow detail, so
  shafts through blinds and branches flicker less.
- **Ground-truth ambient occlusion** (with the ReShade add-on) – GTAO in place
  of the game's HBAO+: deeper shading in corners and creases, none on open
  surfaces, and no dark halos around thin objects.
- **Better-lit faces** – the game's skin light scattering switched on as
  designed (it ships practically off), and character lighting at night with a
  key and a fill light instead of two equal ones.
- **Quality presets** – *Ultra+* and *Cinematic* also raise ambient occlusion,
  fog and rain resolution, light shafts, grass draw distance and model detail.
- **Darker Nights** – optional look where the flashlight really matters.
- **Free camera and photo mode** – the developers' free camera, pause,
  frame-by-frame stepping, slow motion and time of day.
- **In-game controls** (with ReShade) – presets, photo mode and live sliders
  for over 1,000 engine settings.

## Requirements

- Alan Wake Remastered for PC, **Epic Games Store version** (game build 34885).
  This is the only version the mod works with. It does **not** work with the
  original Alan Wake (2010/2012) on Steam, which is a different engine build.
  If the mod doesn't recognise the game it does nothing and says so in
  `awtweaks.log`.
- Windows 10 or 11, 64-bit.
- About 1 GB of extra video memory for the larger shadow maps (can be turned off).
- Optional: [ReShade](https://reshade.me) 6.8 or newer, **"with full add-on
  support"** version, for the in-game control panel.

## Install

1. Open the game folder – the one containing `Game_f_x64_EOS.exe`.
2. Copy **`xinput1_4.dll`** and **`awtweaks.ini`** into it.
3. *Optional, for the in-game panel:* install ReShade with full add-on support
   for `Game_f_x64_EOS.exe` (choose DirectX 10/11/12; effect packs are optional),
   then also copy **`AWTweaks.addon64`** into the game folder.

Start the game. It begins on the **Cinematic** preset.

## Controls

| Key | Action |
|---|---|
| **F10** | Mod on/off – compare with the stock game (high beep = on) |
| **F9** | Cycle quality: Stock → Ultra+ → Cinematic |
| **F7** | Free camera: fly → frozen (Alan playable) → off |
| **F8** | Pause / resume |
| **F6** | Advance one frame (while paused) |
| **Minus / Plus** | Slower / faster game speed (0.01× – 4×) |
| **Backspace** | Normal speed |
| **PageDown / PageUp** | Time of day −/+ 1 minute (hold Shift for 1 hour) |

While flying the free camera: move with your movement keys or left stick, look
with the mouse or right stick, **Space** up, **Left Ctrl** down, **Left Shift**
faster, **Left Alt** slower. Alan stays where he is.

With ReShade, press **Home** and open the **AWTweaks** tab. Its own tabs hold
*Presets* (with descriptions and an FPS comparison), *Lighting* (sliders for
the sun, flashlight and ambient-occlusion effects), *Photo mode* and
*Advanced* (every engine setting). Slider edits apply at once; a bar at the
top counts unsaved changes and saves them to `awtweaks.ini`.

Every key can be changed in `awtweaks.ini`.

## Presets

| | Stock | Ultra+ | Cinematic |
|---|---|---|---|
| Sun/moon shadow detail | 2048 | 3072 | 4096 |
| Distant objects cast shadows | some | some | all |
| Ambient occlusion strength | stock | stronger | stronger |
| Ground-truth AO (GTAO)\* | off | 1.0 | 1.5 |
| Fog and rain | half resolution | full | full |
| Grass draw distance | 60 m | 90 m | 240 m |
| Model detail distance | stock | 1.7× | 3.3× |
| Shadow edge softness (cascade filter size) | 9 | 11 | 13 |
| Soft shadows (sun size)\* | off | 0.5° | 1.0° |
| Contact shadows\* | off | 0.3 m | 0.6 m |
| Flashlight softness (lens radius)\* | off | 2 cm | 3 cm |
| Beam quality\* | off | 1 | 2 |
| Skin light scattering | practically off | on | on |
| Character lights at night (key / fill) | 0.31 / 0.31 | 0.5 / 0.1 | 0.5 / 0.1 |
| **Frame-rate cost vs Stock** | – | **about 16%** | **about 22%** |

\* Need the ReShade add-on. Adjust them with the *Sun and moon shadows*,
*Flashlight shadows*, *Flashlight beams* and *Ambient occlusion* sliders in the overlay.

Costs were measured with the overlay's *Measure FPS of each* button (4K,
paused, same view), with `VolumetricShader=Enhanced`: Stock 148.2, Ultra+ 124.0,
Cinematic 115.1 FPS. Beam quality accounts for about 0.25 ms of that at 1 and
0.35 ms at 2 (an earlier run without it: Ultra+ 128, Cinematic 120). Without the
soft and contact shadows the presets cost about 5% and 10%; GTAO costs too
little to measure. Your numbers will vary with hardware and scene;
run the same button to check yours.

*Darker Nights* can be combined with any of them: less automatic brightening at
night, no invisible light following Alan, dimmer fill lights on characters, and
a darker sky and fog. Its dimmer character lights (key 0.2, fill 0.04) replace
the presets' night values.

Cinematic is the heaviest. If your frame rate drops, use Ultra+, turn the
soft and contact shadow sliders down (0 = off), or set `AtlasScale=1` under
`[Shadows]` in `awtweaks.ini` (keeps the other improvements but caps shadows
at the stock 2048).

## Settings file

`awtweaks.ini` is re-read while the game runs – save it and changes apply
almost immediately (except settings marked "read at game start"). It explains
each option in its comments. Your own changes go under `[Tweaks]`;
`awtweaks_dump.txt`, written next to it, lists every engine setting with its
stock value.

## Uninstall

Delete `xinput1_4.dll`, `AWTweaks.addon64`, `awtweaks.ini`, `awtweaks.log`,
`awtweaks.prev.log` and `awtweaks_dump.txt` from the game folder. The game
itself is never modified on disk.

## Troubleshooting

- **Nothing changes in game:** check `awtweaks.log` in the game folder. "Mod
  idle" or "signature not found" means this game version isn't supported.
- **Frame rate too low:** switch to Ultra+ (F9), turn down the soft and contact
  shadow sliders, or set `AtlasScale=1`.
- **Shadows, flashlight or ambient occlusion look wrong after a game update:**
  set `ShadowShader`, `SpotShader` or `AOShader` to `Original` under
  `[Shaders]` in `awtweaks.ini` to use the game's own shader.
- **Out of video memory / stutter:** set `AtlasScale=1` under `[Shadows]`.
- **Controller conflicts:** the mod passes all controller input through to the
  game; it only adds keyboard keys. Change any key in `awtweaks.ini`.

## Known issues

- **With ReShade installed, Windows logs a crash when the game closes.** This
  is ReShade on this game – it happens with the mod removed too – and only
  after the game has finished shutting down; nothing is lost.
- **Time of day** is ignored in scenes that script their own lighting.
- **The FPS comparison** shows no difference if your frame rate is capped
  (V-Sync, NVIDIA Reflex or a driver limit); lift the cap while measuring.

## License

MIT – see `LICENSE`. Includes ReShade API and Dear ImGui headers, see
`THIRD_PARTY_NOTICES.txt`. Technical notes on how the mod works are in
`docs/TECHNICAL.md` in the source repository.

Not affiliated with Remedy Entertainment.
