<img src="pcsx2-wasm/web/favicon.svg" width="44" alt="">

# TabStationToo

TabStationToo runs PlayStation 2 emulation in a browser tab. It is the
[PCSX2](https://github.com/PCSX2/pcsx2) emulator compiled to WebAssembly, with a page of its own
around it. You bring the BIOS from your own console and disc images of your own games; nothing is
uploaded anywhere.

**Try it:** https://tabstationtoo.github.io/TabStationToo/ (desktop Chrome or Edge; Firefox works
too).

TabStationToo is built on PCSX2 and would not exist without the PCSX2 team and its contributors,
who have developed it for more than twenty years. It is an independent project: it is not
affiliated with, endorsed by or supported by the PCSX2 project or Sony Interactive Entertainment.
Please do not report TabStationToo problems to PCSX2.

## Status

Early and experimental, but games play.

- **Games** boot from disc images (`.iso`, `.bin`, `.img`, or compressed `.chd`, `.cso`, `.zso`, `.gz`), read straight from your
  file. Tested so far: Dragon Ball Z: Budokai Tenkaichi 3, Guitar Hero, Naruto Shippuden: Ultimate
  Ninja 5, Grand Theft Auto: San Andreas, Final Fantasy XII and WWE SmackDown vs. Raw 2011. Saves on
  memory cards are kept in the browser (or in `.ps2` files on your disk in Chromium browsers).
- **Speed:** many scenes now run at or near full speed in Chrome. Guitar Hero plays a song at
  100 % with rare dips, GTA San Andreas averages about 99 % on foot, and Final Fantasy XII's
  cutscenes run at 100 %. Heavy scenes need a fast CPU: a WWE SmackDown vs. Raw 2011 match runs
  smoothly on a Ryzen 7 7840U laptop at full power. (On Linux, check that a power manager such as
  TLP is not capping the CPU on mains power: an earlier 50-60 % reading of that match came from
  exactly that.) Every measurement is in
  [docs/web-port/PERFORMANCE.md](docs/web-port/PERFORMANCE.md).
- **The disc library** is the home page: choose your BIOS and a folder of disc images, pick a disc
  from the shelf (mouse, keyboard or gamepad) and put it in a PS2 whose tray opens. One insert in
  ten, the disc needs a clean first. With "reduce motion" set in your system (on Linux, GNOME's
  animations switched off), the animations are skipped.
- **Graphics:** PCSX2's hardware renderer on WebGPU, with the software renderer as a fallback;
  native or 2× internal resolution, and Smooth, Crisp, CRT (the CRT-Geom shader, as in ShaderGlass) or CRT 2 (the same with glow and short trails) pictures. The
  picture sits in a drawn CRT television, above a drawing of the console's front.
- **Sound:** PCSX2's sound chip emulation (SPU2) through a Web Audio worklet. Below full speed it is
  time-stretched, as in PCSX2 on the desktop. Browsers only start sound after a key press or click.
- **Controls:** the keyboard and gamepads (Xbox, DualShock, DualSense, Switch Pro and other standard
  pads; any other gamepad can be mapped), each as Player 1 or Player 2. Every key and button can be
  changed by clicking it on the controller drawn under the console. In Firefox and Zen, set
  `dom.gamepad.non_standard_events.enabled` to true in `about:config`, or quick taps can be lost.
- **Underclock:** PCSX2's EE cycle rate, chosen per game, makes heavy scenes faster at the risk of
  slow motion or glitches. Off unless chosen.
- **Save states:** STATE on the remote (or F1 to save, F3 to load the newest) keeps up to 10 states
  per game in the browser, each with a picture of the moment; a copy can be saved to disk as a `.p2s`.
- **Discs change while a game runs** (the disc menu, or a disc dropped on the page), for games on
  several discs: the tray opens, the new disc goes in, the game goes on.
- **Rumble:** the game's vibration plays on Xbox, DualShock 4 and DualSense pads in Chrome and Edge
  (a Rumble checkbox under each controller turns it off).
- **Not there yet:** CHD images with a parent, multi-file BIN/CUE images, and phones.
- **Linux:** Chromium-based browsers keep WebGPU off for most GPUs, so the page falls back to
  software drawing. Graphics → Check WebGPU explains how to turn it on (chrome://flags, "Unsafe
  WebGPU Support" and "Vulkan").

## How it works

- PCSX2's core is compiled with [Emscripten](https://emscripten.org) to WebAssembly, on threads
  backed by `SharedArrayBuffer`.
- A browser cannot run PCSX2's recompilers, which write x86 or ARM code. TabStationToo has its own
  JITs instead: the EE (main CPU), IOP (I/O CPU), VU0 and VU1 (vector units) are compiled to
  WebAssembly modules at run time, and checked bit for bit against PCSX2's interpreters
  ([docs/web-port/VU1-JIT-DESIGN.md](docs/web-port/VU1-JIT-DESIGN.md)).
- The PS2's graphics are drawn by PCSX2's hardware renderer ported to WebGPU
  ([docs/web-port/WEBGPU-DESIGN.md](docs/web-port/WEBGPU-DESIGN.md)). Without WebGPU, PCSX2's
  software renderer draws on a 2D canvas.
- The PS2's sound is mixed by PCSX2 on the emulated CPU's thread and played by an Emscripten Wasm
  Audio Worklet ([docs/web-port/AUDIO-DESIGN.md](docs/web-port/AUDIO-DESIGN.md)).
- The page (`pcsx2-wasm/web/`) is plain HTML, CSS and JavaScript with no build step. It makes no
  third-party requests except disc covers from the
  [ps2-covers](https://github.com/xlenore/ps2-covers) collection, fetched by serial. The BIOS you
  pick stays in your browser (IndexedDB).
- A disc image is never copied: a worker reads the parts the game asks for from your file and
  hands them to the emulator through shared memory
  ([docs/web-port/DISC-DESIGN.md](docs/web-port/DISC-DESIGN.md)). Memory cards come from a
  library the page keeps in IndexedDB, or from `.ps2` files on your disk in Chromium browsers; two
  slots, plugged in and out while a game runs.

The porting plan, with the measurements and decisions behind it, is in
[docs/web-port/PLAN.md](docs/web-port/PLAN.md). The interface design is in
[docs/web-port/UI-SPEC.md](docs/web-port/UI-SPEC.md).

## Build

Requirements: [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) 6.0.12,
CMake and Ninja, and Python 3. The dependencies and a Release build take about 1 GB of disk space,
plus Emscripten itself.

```sh
source ~/emsdk/emsdk_env.sh

# Once: build the third-party libraries for WebAssembly into deps-wasm/
tools/wasm/build-dependencies-wasm.sh deps-wasm

emcmake cmake -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_TESTS=OFF \
  -DCMAKE_PREFIX_PATH="$PWD/deps-wasm" -DCMAKE_FIND_ROOT_PATH="$PWD/deps-wasm" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja -C build-wasm pcsx2-wasm
```

`build-wasm/bin/` is then the web root: `index.html`, the page's assets, and
`pcsx2-wasm.js`/`.wasm`/`.data`.

## Run

Browsers only allow the emulator's shared-memory threads on a cross-origin isolated page, so
opening `index.html` from disk does not work. Use the included dev server, which sends the
required headers and listens on localhost only:

```sh
python3 tools/wasm/serve.py
```

Open http://127.0.0.1:8080/. The disc library opens: choose your BIOS file and the folder with
your disc images (or drop them on the page), then put a disc in. To start without a disc, close the
library (Esc) and press Boot: the console's own menus appear. Click the screen to give it the keyboard,
and press Esc to take the keyboard back. The controllers drawn under the console show and change
the controls.

More detail, including the frontend's API and testing options, is in
[pcsx2-wasm/README.md](pcsx2-wasm/README.md). Its "Deploying" section covers hosting the page
elsewhere, including on GitHub Pages, which cannot send those headers.

## BIOS and games

A PS2 BIOS is copyrighted software. TabStationToo does not include one and never downloads one:
dump it from a console you own (see PCSX2's
[BIOS dumping guide](https://pcsx2.net/docs/setup/bios/)). The same applies to games: use disc
images you made from discs you own. Please do not share BIOS files or game images in issues or
anywhere else in this repository.

## Where things are

| Path | What it is |
|---|---|
| `pcsx2-wasm/` | The browser frontend: `Main.cpp` (the emulator's entry point and the API the page uses) and `web/` (the page; `web/library/` is the disc library, console, TV and controllers) |
| `pcsx2-headless/` | A windowless runner for Node.js, used to test and benchmark without a browser |
| `pcsx2/wasm/` | The WebAssembly JITs (EE, IOP, VU0, VU1) and their fuzzers and lockstep checks |
| `pcsx2/GS/Renderers/WebGPU/` | The WebGPU renderer; its shaders are in `bin/resources/shaders/webgpu/` |
| `common/Emscripten/`, `pcsx2/CDVD/Emscripten/`, `pcsx2/DEV9/Emscripten/` | WebAssembly platform code for PCSX2's core (`CDVD/Emscripten/` reads discs through the page's disc worker) |
| `tools/wasm/` | Dependency build script, the dev server, the crowd benchmark (`bench/`), and the scripts that assemble and smoke-test the deployed site |
| `docs/web-port/` | Porting plan, interface design, the design of each part, and the performance record |
| `docs/upstream/` | PCSX2's own guidance kept for reference (its agent guidelines) |
| everything else | PCSX2, with changes for WebAssembly where needed |

## Licence and credits

TabStationToo is free software under the GNU General Public License v3.0, like PCSX2: see
[COPYING.GPLv3](COPYING.GPLv3). Its source is this repository.

- [PCSX2](https://github.com/PCSX2/pcsx2) and its contributors: the emulator at the heart of
  this project. Upstream code keeps its original copyright notices.
- [Archivo](https://github.com/Omnibus-Type/Archivo) and
  [Chivo Mono](https://github.com/Omnibus-Type/Chivo) by Omnibus-Type, SIL Open Font License 1.1.
- [Phosphor Icons](https://phosphoricons.com), MIT License.
- [coi-serviceworker](https://github.com/gzuidhof/coi-serviceworker) by Guido Zuidhof, MIT License.
- The third-party libraries PCSX2 uses are listed with their licences in
  [bin/docs/ThirdPartyLicenses.html](bin/docs/ThirdPartyLicenses.html).

"PlayStation" and "PS2" are trademarks of Sony Interactive Entertainment Inc. They are used here
only to describe what the software emulates.
