# Changelog

## 0.9.3 (pre-release)

- **Dark triangles on characters under lamps** (Jenny's shoulder,
  [#6](https://github.com/invinceble55-wq/Estacado/issues/6)): the lamps that
  cast shadow maps rebuild each receiver's position from the resolved scene
  depth at the pixel centre, but a depth resolve of 2x MSAA copies one sample,
  a quarter pixel away. On surfaces at a steep angle to the camera that put the
  receiver behind the surface by about the shadow bias, so whole triangles
  shadowed themselves. Depth resolves of 2x MSAA buffers now store the mean of
  both samples where the pixel lies on one surface with its neighbours
  (`resolve_depth_pixel_center`, on); silhouettes keep the sample. The
  shadow pass's constant polygon offset is also applied as the absolute
  depth offset it is on the Xbox 360 (`d3d12_absolute_polygon_offset`).
- **Low latency (new, on by default):** with VSync the game stays at most two
  frames ahead of the display (frame queue 2, the wait ends half a frame
  before the awaited present): 144 Hz VSync at 144 FPS 24.9 -> 16.8 ms input
  to display in the Chapter 1 ride; no change at 60 FPS; off without VSync.
  *Settings > Performance / Frame Rate > Low latency*
  (`display.low_latency`).
- **Widescreen on 16:10 and 4:3 screens:** the bloom rules follow a taller
  guest mode (1280 x 800, 1280 x 960) with *Fill wide and tall screens* on
  ([#17](https://github.com/invinceble55-wq/Estacado/pull/17), thanks David
  Janice).
- **Installed copies:** logs, crash reports, the session log, language packs,
  an extracted disc image and the game-location file go to
  `%LOCALAPPDATA%\Estacado`, so copies under read-only folders work; a portable
  copy (`portable.txt`) keeps everything in its folder; packs and game
  locations set up by earlier versions are still found
  ([#19](https://github.com/invinceble55-wq/Estacado/pull/19), thanks David
  Janice).
- **"Settings unavailable" in the launcher** shows Windows' own reason when
  `TheDarkness.exe` cannot start, and what to do.
- **Bug reports:** session logs name the graphics card and its driver; a
  startup line `REX_GPU_TEST_SWITCHES` lists the GPU test switches in use.
- **Test switches for picture problems on some graphics cards**
  ([#16](https://github.com/invinceble55-wq/Estacado/issues/16),
  [#20](https://github.com/invinceble55-wq/Estacado/issues/20)):
  `d3d12_conservative_sync = true` puts a full GPU barrier before and after
  every clear, copy, dispatch, resolve and draw (slow; tells missing
  synchronisation apart from other causes); native-grid rule strings without
  the bloom rules turn the bloom fix off;
  `d3d12_render_target_uncompressed = true` creates single-sample colour
  render targets with simultaneous access, which keeps the graphics card from
  compressing them; `d3d12_transfer_stencil_clear_by_draw = true` (with the
  existing `depth_transfer_not_equal_test = false`) clears the stencil of
  depth buffer copies by drawing instead of a rectangle clear. The startup
  line `REX_GPU_TEST_SWITCHES` lists them. Developer diagnostic
  `d3d12_debug_gpu_spin` (off) delays every frame's GPU work to emulate a
  slower graphics card.
- ROV path (Intel): pixels rejected by depth/stencil, alpha test or alpha to
  coverage no longer count towards occlusion queries.
- GPU code built with a profile retrained on the current code
  (`config/pgo/rexgpu-v496.profdata`).

## 0.9.2 (pre-release)

Hotfix for regressions in 0.9.1
([#16](https://github.com/invinceble55-wq/Estacado/issues/16)).

- **Controller with *Keyboard / mouse* off:** 0.9.1 answered the game's
  "is a controller connected?" with no until a state poll had chosen a pad,
  and the game polls only connected controllers, so a controller worked only
  while keyboard and mouse were on. The answer now comes from the connected
  pads (the active one first, else the lowest connected port).
- **Bloom with motion blur on at 2x/3x/4x internal resolution:** shimmering
  squares and ripples in menus and gameplay and banded light flashes in the
  main menu. The final composite of the frame reads the console-resolution
  glow through texture fetch 1; 0.9.1 enlarged it smoothly only for the
  composite shader without motion blur (22FC55CE134777AC), not for the
  motion-blur one (A59B41D0BD79484B). Both have the rule now; settings files
  with the 0.9.0 or 0.9.1 rules are upgraded when the game starts. Main menu
  flash at 2x, motion blur on: the share of the brightness change made in the
  steepest tenth of rows fell from 0.27 to 0.19, the same as with motion
  blur off.
- **Button prompts with a controller:** mouse sensor jitter and small
  stick-mode mouse deflections switched the prompts to keyboard keys; now
  only deliberate mouse movement (40 counts within 300 ms) or a stick-mode
  deflection beyond 8000 counts does.
- **Enter acts as the A button** while no key binding uses it, so menus
  confirm with Enter as well as E (Space has been jump since 0.9.1).
- **Back + Start** on a controller opens and closes the in-game settings
  (F1); D-pad or stick, A, LB/RB and B navigate them; the game never sees the
  chord, a lone Start still pauses
  ([#7](https://github.com/invinceble55-wq/Estacado/issues/7)).
- **F9 snapshot** for bug reports: a screenshot plus a one-frame text
  description of the draws (shader hashes, render target, blend, depth and
  scissor state, texture fetch constants, resolves).
- **Opt-in session log:** an empty `session_log.txt` next to
  `TheDarkness.exe` sends the run's log to `logs\session_<date>_<time>.log`.
  New lines for input problems: keyboard focus changes and the first key
  presses (`REX_MNK_FOCUS`, `REX_MNK_KEY`, `REX_SDL_KEY`,
  `REX_INPUT_KEY_DROPPED`) and player 1's controller answer
  (`RUNTIME_INPUT_CAPABILITIES`).
- New release check before every release: keyboard and controller with
  real key presses (Windows `SendInput`, hardware scan codes) and a virtual
  controller (developer switch `DARKNESS_TEST_VIRTUAL_PAD=<port>`): fresh
  settings, keyboard/mouse off with and without a controller, both on, and a
  0.9.0 settings file.
- GPU code built with a profile retrained on the current code
  (`config/pgo/rexgpu-v467.profdata`).

## 0.9.1 (pre-release)

- **Bloom at 2x/3x/4x internal resolution** no longer bands or streaks around
  lights: the glow is made at the console's resolution and enlarged smoothly,
  in both of the game's anti-aliasing modes. By djanice1980
  ([#11](https://github.com/invinceble55-wq/Estacado/issues/11),
  [#15](https://github.com/invinceble55-wq/Estacado/pull/15),
  [Estacado-ReXGlue #1](https://github.com/invinceble55-wq/Estacado-ReXGlue/pull/1);
  fixes [#1](https://github.com/invinceble55-wq/Estacado/issues/1)).
- **Pause menu colours at 2x and 4x** internal resolution: the greenish haze
  over the paused game is gone. The pause menu's colour-grading table was
  drawn at the scaled resolution and read at the console's grid, which only
  lines up at odd scales; it is now drawn at the console's resolution like
  the game's other grading tables
  ([#10](https://github.com/invinceble55-wq/Estacado/issues/10)).
- **No more yellow flashes in the Chapter 1 car ride:** 0.9.0 showed single
  over-exposed yellow frames there (35 to 45 in each 140-second recording of
  the ride at 2x internal resolution); 0.9.1 showed none in four recordings.
- **Controllers on any port:** a controller that Windows, Steam Input or a
  virtual-pad driver puts on a port other than the first now plays as the
  player with the profile, so the profile and saves load and keyboard and
  controller can be switched freely
  ([#8](https://github.com/invinceble55-wq/Estacado/issues/8)).
- **Saves and settings in Saved Games** (`Saved Games\Estacado`), so updates
  never lose progress. The first start in a 0.9.0 folder copies its saves and
  settings there, checks every file and keeps the originals; the launcher can
  import saves from another folder; `portable.txt` keeps everything in the
  game folder. Suggested by djanice1980
  ([#7](https://github.com/invinceble55-wq/Estacado/issues/7)).
- **Other releases of the game** run when they contain the same code (for
  example localised releases): the launcher compares the decrypted
  executable, not the file. Other versions get a plain message and a local
  report ([#9](https://github.com/invinceble55-wq/Estacado/issues/9)).
- The launcher names any file missing from an incomplete extraction, and the
  zip now stores its folders explicitly
  ([#2](https://github.com/invinceble55-wq/Estacado/issues/2)).
- Folders and Windows user names with letters outside the system's code page
  work for settings and saves.
- **Quit game** button in the in-game settings (F1), with a confirmation
  (djanice1980, [#7](https://github.com/invinceble55-wq/Estacado/issues/7)).
- **Key bindings name each button's action** in the game's default controller
  layout, for example *Y button: jump* and *A button: use*, so a key can be
  moved to the action you want
  ([#12](https://github.com/invinceble55-wq/Estacado/issues/12)).
- **Jump is on Space and use on E** by default, as in most PC games (0.9.0 had
  them the other way round). Settings from 0.9.0 get the new keys if those two
  were never changed; keys you chose yourself stay as they are.
- **Button prompts follow your keys:** while you play with keyboard and mouse,
  the game's button icons show the keys you bound (for example *E*, *Space*,
  *Shift*, *LMB*) and prompt texts read like "Press E"; the Xbox buttons come
  back as soon as you use a controller. *Button prompts* in the settings
  (Automatic, Xbox buttons, Keyboard keys) changes this, also during play.
- **Less stutter in new areas on the first play:** the release now carries a
  data-free list of the game's shaders and pipelines: shader hashes, pipeline
  render states and, per shader, which shader of the game's own
  `System\Xenon\ProgramCache.xpc` it is plus the vertex-fetch bindings the
  console's Direct3D patches in (no shader code). At startup the shaders are
  rebuilt from the player's copy, each checked against its hash, and every
  pipeline the list names is compiled in the background, with a small
  progress note. First start with cold caches, spawn to the Chinatown street:
  16 compile waits (170 ms) before, 1 (2 ms) now. Shaders the game creates
  that are not on the list are still found in memory and compiled before
  their first draw when possible
  ([#5](https://github.com/invinceble55-wq/Estacado/issues/5)).
- Developer thread snapshots are no longer written to `logs` at every start.

## 0.9.0 (pre-release)

The first public pre-release. Tested on one Windows 11 PC with an NVIDIA RTX
graphics card; other hardware is untested, so reports are very welcome (see
[Reporting a problem](README.md#reporting-a-problem)).

- Native Windows port of *The Darkness* (Xbox 360, USA/Europe disc) by
  static recompilation, with a Direct3D 12 renderer.
- First start: the launcher takes your disc image or extracted game folder,
  checks that it is the supported version and prepares it.
- Frame rate: Original 30, 60, the display's refresh rate, a custom limit or
  uncapped, with scripted scenes, physics and audio at their original speed.
- Resolution: 1x/2x/3x internal scale or automatic, any output resolution,
  windowed or borderless.
- Anti-aliasing: SMAA (default) or FXAA; AMD FSR 1 or CAS when the image is
  fitted to the screen.
- Keyboard and mouse with native mouse look and rebindable keys; controller;
  field of view.
- Settings in the launcher and in an in-game overlay (F1); presets Enhanced,
  Performance, Original and Steam Deck.
- Motion blur on or off and HD texture packs.
- Languages: the game's five (English, French, German, Italian, Spanish).
- Widescreen for 21:9, 32:9 and 16:10 screens (experimental, off by default).
- Crash reports (log and minidump) in the `logs` folder.
