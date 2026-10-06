# Estacado: a native PC recompilation of The Darkness (Xbox 360)

An unofficial fan port of *The Darkness* (Xbox 360, 2007), made by static
recompilation: the game's PowerPC code is translated into C++ and runs
natively on Windows, with a Direct3D 12 graphics layer built on
[ReXGlue](https://github.com/rexglue/rexglue-sdk) and
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

This is a free, non-commercial fan project. **No game data is included:** you
need your own copy of the game (see [Game files](#game-files) and
[Legal](#legal)).

> **Pre-release.** So far it has been tested on one Windows 11 PC with an
> NVIDIA RTX graphics card. Other hardware (AMD, Intel, older NVIDIA, laptops)
> is untested. Please report what you find.

**Steam Deck:** a player reports that with 0.9.1 both the settings launcher
and the game work on the Deck under Proton: the launcher starts there and
launches into the game ([issue #2](https://github.com/invinceble55-wq/Estacado/issues/2)).
A Steam Deck preset is included. If you copy the folder to a Deck, see
[Saves and settings](#saves-and-settings) to take your saves along.

## What it offers

- **Frame rates beyond 30**: 60, your display's refresh rate (120/144/240),
  a custom limit or uncapped, with the game's timing kept correct (scripted
  scenes, physics and audio run at their original speed).
- **Resolution**: the 3D scene at 1x, 2x or 3x the console's 1280 x 720, or
  automatic (the launcher measures your graphics card once), fitted to any
  output resolution; windowed, borderless or automatic.
- **Anti-aliasing**: SMAA (default) or FXAA; AMD FSR 1 or CAS sharpening when
  the image is fitted to your screen. The game's own 4x MSAA stays on all the
  time by default (the console dropped to 2x when it ran slowly).
- **Widescreen (experimental)**: fills 21:9, 32:9 and 16:10 screens with more
  view at the sides; menus stay centred.
- **Controls**: keyboard and mouse with native mouse look and rebindable
  keys (on-screen prompts show your keys), or a controller; adjustable field
  of view.
- **Settings** in a launcher (presets: Enhanced, Performance, Original, Steam
  Deck) and in an overlay over the running game (F1).
- **Motion blur on/off** and support for **HD texture packs**: folder-based
  replacements made by players. No pack is included or provided (the game's
  textures belong to its publisher); [docs/HD_TEXTURE_PACKS.md](docs/HD_TEXTURE_PACKS.md)
  explains how to install one and how to make one.

## Screenshots

Our own captures from test runs of this port.

![The Chinatown street at 2x internal resolution (2560 x 1440)](docs/screenshots/chinatown-street.jpg)

![Fulton Street subway station](docs/screenshots/fulton-street-station.jpg)

![The launcher: Graphics Quality settings](docs/screenshots/launcher.jpg)

## Requirements

- Windows 10 or 11, 64-bit.
- A CPU with AVX2 (Intel Haswell / AMD Zen or newer).
- A Direct3D 12 graphics card with current drivers.
- [Microsoft Visual C++ 2015-2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- About 8 GB of free disk space if you use a disc image (it is extracted
  once).

## Game files

The recompiled code is that of this version, the one we own and test:

| | |
|---|---|
| Game | *The Darkness*, Xbox 360, USA/Europe disc (En, Fr, De, Es, It), title ID 545407EE |
| Disc image (Redump) | `Darkness, The (USA, Europe) (En,Fr,De,Es,It).iso`, SHA-256 `46f7305c1e1972e888d3624f15b0ed483923aeeb50ecad0c6bf5b42f69d52a90` |
| `default.xex` | SHA-256 `aace35a8f9bcdc7f28aeab9ff8cf3bdf200353f5c83705f6284487347acb3c5f` |

**Other releases** are accepted when they contain the same code. The launcher
decides this itself: it decrypts your `default.xex` and compares its code,
its structure and the data the code depends on with the version above. Text,
fonts and the game's own strings may differ, so a localised release that only
replaces those works and shows its own language. We can only fully test the
version we own: we checked a Russian release this way (same code, only text
and font data differ) but have not played it through.

A release with different code (another build of the game, or a title update)
does not run yet. The launcher says so plainly and saves a detailed report,
`game_version_report.txt` in the `logs` folder (names, sizes and hashes only,
no game data), that you can attach to an issue if you like. Never upload game
files.

Dump the game from your own disc; this project does not provide or link to
game files.

## Install

Download the zip from the [Releases](../../releases) page and extract it to a
folder of your choice (not inside *Program Files*). Or build it yourself: see
[BUILDING.md](BUILDING.md).

### First start

1. Run `TheDarknessSettings.exe` (the launcher).
2. On the first start it asks for your game: **Choose disc image...** (your
   `.iso`) or **Choose game folder...** (a folder with the extracted files, the
   one that contains `default.xex`). It checks that the version is supported;
   a disc image is extracted into the `game` folder next to the launcher.
3. Pick a preset (Enhanced is recommended), adjust anything you like and press
   **Play**.

The launcher's **About** button shows the version and where your saves are,
and opens the licence texts and the `logs` folder.

### Saves and settings

Your saves and settings are kept in your Saved Games folder,
`%USERPROFILE%\Saved Games\Estacado` (settings: `TheDarkness.pc.toml`; saves:
`content`), apart from the program. An update can go into a new folder or
over the old one without touching your progress.

- **Coming from 0.9.0:** extract the new version over your old folder (or
  into it). On its first start it copies that folder's saves, settings and
  screenshots to Saved Games, checks every copied file, and leaves the
  originals untouched as a backup (a note, `SAVES_MOVED.txt`, says so). If
  the copy cannot be made, the game keeps using the old folder and tries again
  at the next start.
- **0.9.0 in another folder:** in the launcher, **About**, then **Import
  saves...**, and choose the old folder. Saves already in Saved Games are kept
  under a new name, never overwritten.
- **Portable (USB drive, a folder copied to a Steam Deck or another PC):** put
  an empty file named `portable.txt` next to `TheDarkness.exe`. Saves and
  settings then stay inside the game folder (`TheDarkness.pc.toml` and
  `runtime_data`), as in 0.9.0.

## Settings overview

Open the launcher, or press **F1** in the game. Changes to resolution,
widescreen, language and a few graphics options apply at the next start; the
launcher says so for each.

- **Performance / Frame Rate**: *Frame rate* (Original 30, 60, display
  refresh, custom, uncapped), *VSync* (VSync, VRR/G-SYNC/FreeSync,
  immediate), *Menu frame rate*.
- **Display**: output resolution, window mode, monitor, *Fill wide and tall
  screens* (widescreen, experimental).
- **Graphics Quality**: resolution scale, anti-aliasing, upscaling, motion
  blur, HD texture packs.
- **Camera**: field of view.
- **Controls / Key bindings**: keyboard and mouse, mouse look, sensitivity,
  button prompts (automatic, Xbox buttons or keyboard keys), controller
  options.
- **Language**.

## Languages

English, French, German, Italian and Spanish: the game's own languages, for
its text and speech. Set *Language* in the launcher; *Automatic* follows the
Windows display language.

## Known issues

- Tested on one PC only (see the note at the top).
- Played on PC so far: every menu and the campaign from the start up to the
  orphanage explosion scene, including the Chinatown street and its combat.
  The chapters after that have not been played through yet. A check of
  everything the game can ask of the port found no gap on the single-player
  path (details in [docs/WHOLE_GAME_AUDIT.md](docs/WHOLE_GAME_AUDIT.md)); if
  the game stops anyway, it shows a message and saves a crash report: please
  send it.
- Multiplayer: the menus work, but there is no online or local network play.
  The port behaves like a console without a network connection: the Xbox
  LIVE modes and leaderboards ask for an Xbox LIVE sign-in and stay on the
  menu, and System Link finds no games.
- Widescreen (experimental): at 21:9 and wider, subtitles are drawn larger
  than at 16:9 and two-line subtitles can overlap.
- Shaders the release's list does not cover yet (mostly later levels) are
  compiled for your graphics card the first time they appear, which can
  stutter briefly; later sessions reuse them.
- Black dots or flickering dark squares above internal scale 1x on some
  NVIDIA RTX 20 and 30 series cards
  ([#16](https://github.com/invinceble55-wq/Estacado/issues/16),
  [#20](https://github.com/invinceble55-wq/Estacado/issues/20)): they come
  with the game's own switch to 2x anti-aliasing. Since 0.9.5 the game stays
  in 4x (*Graphics Quality > Multisampling (MSAA)*, Always 4x), and the 2x
  mode leaves out the glow smoothing they came with; confirmation from
  affected cards is pending.
- Being looked into: green glitches in the Otherworld until a restart
  ([#14](https://github.com/invinceble55-wq/Estacado/issues/14)) and face
  shading that flickers in some conversations
  ([#4](https://github.com/invinceble55-wq/Estacado/issues/4)).
- Not in this release: HDR output, temporal anti-aliasing with upscalers
  (such as DLSS, FSR 3 or XeSS) and frame generation. If you would use one of
  them, say so in an issue.

## Reporting a problem

Please open an issue with the **Bug report** template. It asks for:

- your CPU, graphics card, driver version, Windows version and screen
  resolution/refresh rate;
- your `TheDarkness.pc.toml` (settings, in `Saved Games\Estacado`) and what
  you did when the problem happened;
- the `logs` folder (launcher: **About**, then **Logs**; usually
  `%LOCALAPPDATA%\Estacado\logs`, or next to `TheDarkness.exe` in a portable
  copy): `runtime_crash.log` and any `TheDarkness_fatal_*.dmp` and
  `TheDarkness_stall_*.dmp` files.

For a visual glitch, press **F9** while it is on screen: the game saves a
screenshot and a small text file describing that frame in
`Saved Games\Estacado\screenshots`; attach both. For input or other
problems without a crash, create an empty file named `session_log.txt` next
to `TheDarkness.exe`, play until the problem happens, and attach the
`session_*.log` it writes in the `logs` folder.

Never attach game files (disc images, extracted files) to an issue.

## Building from source

See [BUILDING.md](BUILDING.md). In short: one script checks your game files,
recompiles your own `default.xex`, builds the runtime and packages it.

## Legal

- **Unofficial.** Estacado is a fan project. It is not affiliated with,
  endorsed or sponsored by 2K Games, Take-Two Interactive, Starbreeze
  Studios, Top Cow Productions or Microsoft.
- **Trademarks** belong to their owners. *The Darkness* and related names are
  trademarks of their respective owners; *Xbox* and *Xbox 360* are trademarks
  of Microsoft. They are used here only to name the game this project works
  with.
- **No game data.** Neither this repository nor its releases contain any of
  the game's data: no textures, models, audio, video, text or other files
  from the disc.
- **What the download contains.** `TheDarkness.exe` contains the game's
  program code in translated form: the original PowerPC code converted into
  native code by static recompilation. It does nothing on its own. It needs
  your own, legally obtained copy of the game, which the launcher checks and
  prepares on your computer. The game's files and everything made from them
  (extracted files, shader caches, saves) stay on your computer: do not share
  them.
- **Non-commercial.** This project is free. It sells nothing and accepts no
  donations or sponsorships.
- **No official artwork.** The repository and the releases contain no logos,
  box art or marketing art of the game; titles are plain text and the
  screenshots are our own.
- **Licences.** Our source code is released under the MIT licence
  ([LICENSE](LICENSE)). The licence covers the source code of this
  repository only: it does not cover the game *The Darkness* or anything
  derived from it, such as the game's files or its recompiled code.
  Third-party components keep their own licences
  ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
- **Rights holders** with a concern about this project: please open an issue
  and we will respond.

## Credits

- [djanice1980](https://github.com/djanice1980): the bloom fix at higher
  internal resolutions (the cause in
  [#11](https://github.com/invinceble55-wq/Estacado/issues/11), the fix in
  [#15](https://github.com/invinceble55-wq/Estacado/pull/15) and
  [Estacado-ReXGlue #1](https://github.com/invinceble55-wq/Estacado-ReXGlue/pull/1)),
  the Quit game button, and the idea of keeping saves outside the program
  folder ([#7](https://github.com/invinceble55-wq/Estacado/issues/7)).
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and the
  [Xenia](https://github.com/xenia-project/xenia) project, whose work this
  port's system and graphics layer builds on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev).
- SMAA (Jimenez et al.), AMD FidelityFX FSR 1 and CAS, Dear ImGui, SDL, and
  the other libraries listed in
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
