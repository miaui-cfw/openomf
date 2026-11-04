# OpenOMF (One Must Fall 2097) for the Anbernic RG35XX (MiaUI fork)

[OpenOMF](https://github.com/omf2097/openomf), the open-source remake of the 1994 fighting game One Must Fall 2097, built for the RG35XX: Cortex-A9 armhf, PowerVR SGX544 (**OpenGL ES 2.0 only**), 640×480.
- **Scope:** branch `rg35xx`. Every change is behind `RG35XX` (C `#ifdef`, or the `rg35xx` renderer plugin in CMake), so upstream's builds are unchanged. The one exception is an upstream bug fix: the missing `opus_load_memory` stub in builds without Opus.
- **Speed:** **60 fps** in fights, every panel refresh drawn, with about 2.7 ms of work per frame.
- **Picture:** the game's 320×200 screen doubled to a pixel-perfect **640×400**, with black bars of 40 lines above and below.

**Game data:** One Must Fall 2097 is freeware. `rg35xx/build.sh assets` downloads the game's files from omf2097.com; they aren't part of this repository.

**License:** OpenOMF is under the MIT License ([LICENSE](../LICENSE)), and so are the RG35XX changes.

MiaUI 🐱 port. The upstream README is in [README.md](../README.md).

## The game on the RG35XX

- **Renderer:** a software renderer composes each frame exactly as upstream's OpenGL 3 renderer does (palette effects, remaps, shadows, translucency, fades), and GLES2 shows it. The device can't run the GL3 renderer.
- **Fight timing:** upstream's own timing at every speed (Gameplay → Speed).
  - At the default speed the fighters update about 36 times a second, so on the 60 Hz panel an update stays up for one or two refreshes, as on any 60 Hz PC screen.
  - `--lock` makes speeds **8** and **4** perfectly even: one fight tick on every refresh (speed 8) or every second refresh (speed 4), about 3% slower than nominal. Other speeds keep upstream's timing either way.
- **Sound:** stereo at 44.1 kHz with a 1024-frame buffer, half upstream's.
  - The Mono option is gone, because this device plays a mono stream at twice its speed.
  - Effects start without a stall. Upstream resampled each one on the main thread, about 12 ms each, a dropped frame per hit.

## The pak

| File | What it is |
|---|---|
| `openomf` | the program (ARM, stripped, `-O3`). **This is what to run.** |
| `resources/` | the game data (One Must Fall 2097, freeware) and OpenOMF's own files (`openomf.bk`, the `*.DAT2`/`*.LNG2` language files, `gamecontrollerdb.txt`, the licenses) |
| `README.md` | this file |

The game creates `saves/` on the first run: `openomf.conf` (all settings), and the tournament pilots, scores and recordings it saves. **Keep `saves/` when you replace the pak.**

## Launching

MiaUI supplies the pak's `launch.sh`. For any other setup, everything it needs is here:

```sh
cd /userdata/Ports/OpenOMF.pak    # optional: the program finds resources/ and saves/ next to itself
./openomf
```

Options, all optional:

| Option | Effect |
|---|---|
| `--lock` | speeds 8 and 4: every fight tick exactly one or two panel refreshes (perfectly smooth) |
| `--speed=N` | the game speed, 1–10 (otherwise the Gameplay menu's setting is kept) |
| `--stats` | frame, fight-tick and input statistics on stderr |
| `--help` | upstream's other options |

## Controls (MiaUI convention: A = confirm, B = back)

| RG35XX | In a fight | In menus |
|---|---|---|
| D-pad | move (special moves are direction sequences) | navigate |
| **A** (right), X (top) | punch | **accept** |
| **B** (bottom), Y (left) | kick | B: **back**; Y: accept |
| START, MENU | pause menu | back |

- **Pause menu:** A accepts and B goes back (B closes it).
- **Held buttons:** a button still held when a menu closes counts again only after it's released, so the A or B that closes the pause menu doesn't also punch or kick.
- **Main menu:** B only moves to QUIT. Quitting takes A on QUIT; then the credits roll, and any button skips them.
- **The pad** is mapped by its GUID in the program, and player one uses "RG35XX Gamepad" by default.

## Performance and memory (measured on the device at 1.2 GHz, `-O3`)

| | |
|---|---|
| Fights | 60.5 fps, 0 late frames per 10 s; late frames only on scene loads |
| Work per frame (game + composition + present) | about 2.7 ms mean, under 5 ms max; compose + present about 1.3 ms |
| CPU in a fight | about 21% of one core: the game and the renderer about 18%, sound about 2% |
| Memory | peak RSS about 21 MB |
| Fight ticks, default speed (28 ms ticks) | 1 tick on about 59% of frames and none on the rest (the inherent 2/2/1 cadence) |
| Fight ticks, speed 8 with `--lock` | 1 tick on 600 of 600 frames |

How the renderer got there:
1. **First build:** 10.5 ms of work in the menus. 78% of the CPU went to integer division in the sprite loop, because the A9 has no divide instruction. Fixed with column and row maps.
2. **About 4.8 ms:** 133k pixels a frame, and a 384 KB working set larger than the cache. Fixed with a flag bit in the index, so a plain pixel is one 2-byte store.
3. **Fights still had 10–26 late frames per 10 s,** with 29–68 ms spikes: each sound effect cost a 12 ms SDL resample on the main thread. Fixed with a linear conversion.

## Building

Requirements: Docker, plus `curl`, `unzip` and `sha256sum` on the host.

```sh
git clone -b rg35xx https://github.com/miaui-cfw/openomf.git
cd openomf
rg35xx/build.sh image        # once: the build image (koriki-soh), downloads the ~800 MB SDK
rg35xx/build.sh release      # -> rg35xx/build/pak/OpenOMF.pak
```

`rg35xx/build.sh` with no arguments prints the help. `release` runs the other steps when their output is missing:

| Command | What it does |
|---|---|
| `rg35xx/build.sh image` | the Docker build image from `rg35xx/Dockerfile` |
| `rg35xx/build.sh deps` | libxmp 4.5.0: downloads the release tarball (SHA-256 checked) and cross-builds it static into `rg35xx/build/deps`. Upstream needs libxmp ≥ 4.5 and the device has 4.4.1, so it's linked in. |
| `rg35xx/build.sh assets` | the game's `resources/` into `rg35xx/build/resources` (below) |
| `rg35xx/build.sh arm` | configures and cross-compiles into `rg35xx/build/arm-Release/openomf`, plus the stripped `openomf.stripped` |
| `rg35xx/build.sh release` | `arm` at `-O3`, then the pak in `rg35xx/build/pak/OpenOMF.pak`: the stripped binary, `resources/` and this README |

**`assets`** assembles `resources/` as an OpenOMF release ships it, all from public sources:
- **The game's files:** upstream's [openomf-assets.zip](https://www.omf2097.com/pub/files/omf/openomf-assets.zip) (One Must Fall 2097, freeware), downloaded and SHA-256 checked.
- **The language files** (`ENGLISH.DAT2`, `GERMAN.DAT2`, `DANISH.LNG`, `DANISH.LNG2`): made from `resources/*.TXT` by upstream's `tools/languagetool`, built for the host. `rg35xx/languagetool-log.c` stands in for upstream's logger, which needs SDL.
- **Upstream's own files:** `openomf.bk`, `gamecontrollerdb.txt`, the icon, the desktop file, `README.md` and the licenses.

Settings, all optional and taken from the environment:
- `BT`: the CMake build type for `arm` (default `Release`).
- `OPT`: the optimisation level, added after upstream's `-O2` through `rg35xx/rg35xx-opt.cmake` (`CMAKE_PROJECT_INCLUDE`), so it's the one that counts (default `-O2`; `release` always uses `-O3`).
- `RES`: the resources for `release` (default `rg35xx/build/resources`), e.g. an existing OpenOMF install's `resources/`.
- `TOOLCHAIN_IMAGE`: the image name (default `koriki-soh`).

What the build does:
- **The image** (`rg35xx/Dockerfile`) is self-contained: Debian 12 and the public Batocera RG35XX SDK (buildroot GCC 12.2, armhf, SDL2, SDL2_mixer, enet, libpng and GLES2/EGL in the sysroot) in `/opt/rg35xx`, plus CMake 3.30 and Ninja. It's the same image as the MiaUI Shipwright, Sonic Mania and OpenTyrian ports.
- **CMake** with `rg35xx/rg35xx.cmake` (the toolchain, `-marm -mcpu=cortex-a9 -mfpu=neon-fp16 -mfloat-abi=hard -DRG35XX`) and these options: `-DOMF_VIDEO_RENDERERS=rg35xx -DOMF_AUDIO_BACKENDS=sdl -DUSE_LIBPNG=ON -DUSE_OPUSFILE=OFF -DUSE_ZIP=OFF -DENABLE_MODMANAGER=OFF -DUSE_MINIUPNPC=OFF -DUSE_NATPMP=OFF -DUSE_CONFUSE=OFF -DBUILD_LANGUAGES=OFF -DUSE_COLORS=OFF`. `USE_LIBPNG` stays on, because upstream doesn't compile without it.
- **The binary** is about 1.0 MB stripped. It needs libSDL2, libSDL2_mixer, libenet, libpng, libz and libGLESv2, all on the device.

## What this fork changes

| File | Change |
|---|---|
| `video/renderers/rg35xx/` (new), `video.c`, `CMakeLists.txt` | **The renderer:** upstream's GL3 paletted pipeline on the CPU, at 320×200. A 16-bit index per pixel; the remap, translucent and additive channels are packed in one word, kept only for pixels whose index has a flag bit. Sprites are drawn as submitted, following the GL3 blend modes' channel rules (nearest sampling, flips, scaling, palette offset and limit, remap lookup, 4-sample shadow coverage by maximum, noise-dithered opacity). The resolve follows `rgba.frag`. RGB565 goes into three textures in turn (the SGX copies a texture a frame in flight still uses), drawn at 2×. `render_area_*` reads back indexes like the GL3 renderer. All buffers are black on the way in and out. `--stats` frame statistics. |
| `utils/config.c` | **Settings without libconfuse:** the options in a `name = value` file (libconfuse's syntax), loaded at `conf_init`, written only when the text changes, through `.new` and a rename. |
| `resources/resource_paths.c` | Settings and state in `<exe>/saves` (after `OPENOMF_*_PATH`); resources were already found next to the binary. |
| `main.c` | `--lock`, `--stats`; no log file, the messages on stderr. |
| `engine.c`, `engine.h` | `--lock`: at 16/32 ms ticks, one fight tick per one or two presented frames. `--stats`: slow tick loops logged, a fight-ticks-per-frame histogram. |
| `audio/backends/sdl/sdl_backend.c` | Always stereo; 1024-frame buffer; 44.1 kHz first. Effects converted by linear interpolation (SDL's sinc resampler took about 12 ms per effect on the main thread). The channel is halted before its chunk is freed. |
| `game/utils/settings.c` | Defaults: 44.1 kHz; player one on the "RG35XX Gamepad". |
| `game/scenes/mainmenu/menu_audio.c` | No MONO entry. |
| `controller/game_controller_db.c` | The RG35XX pad's mapping from its GUID (A b0, B b1, X b2, Y b3, START b9, MENU b10 = guide, d-pad b13–16). |
| `controller/joystick.c`, `.h` | Punch A/X, kick B/Y, escape START/MENU. B is the escape everywhere but in a fight (arena, not paused, not a demo), for menu and player polls alike. Attack buttons held from a menu count in a fight only once released. `--stats` input log. |
| `game/gui/menu.c`, `.h`, `mainmenu/menu_main.c` | The main menu: ESC (B) only selects QUIT. |
| `audio/music_sources/opus_source.c` | The missing `opus_load_memory` stub for builds without Opus (an upstream fix, not RG35XX-only). |

## Credits and licenses

- **One Must Fall 2097** was developed by Diversions Entertainment and published by Epic MegaGames (1994); it's freeware.
- **OpenOMF** is by Tuomas Virtanen, Andrew Thompson, Hunter and others, under the MIT License ([LICENSE](../LICENSE)). Its vendored libraries and the SDL game controller database keep their own licenses (copied into `resources/`).
- **libxmp** 4.5.0 is under the GNU LGPL, version 2.1 or later. It is linked in statically; this repository and `rg35xx/build.sh` are the complete source and build of the binary, so it can be rebuilt and relinked against another libxmp (LGPL §6).
- **The RG35XX changes** in this branch are under OpenOMF's MIT License.
