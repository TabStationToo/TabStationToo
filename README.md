<img src="pcsx2-wasm/web/favicon.svg" width="44" alt="">

# TabStationToo

TabStationToo runs PlayStation 2 emulation in a browser tab. It is the
[PCSX2](https://github.com/PCSX2/pcsx2) emulator compiled to WebAssembly, with a page of its own
around it. You bring the BIOS from your own console and disc images of your own games; nothing is
uploaded anywhere.

**Try it:** https://tabstationtoo.github.io/TabStationToo/ (desktop Chrome or Edge).

TabStationToo is built on PCSX2 and would not exist without the PCSX2 team and its contributors,
who have developed it for more than twenty years. It is an independent project: it is not
affiliated with, endorsed by or supported by the PCSX2 project or Sony Interactive Entertainment.
Please do not report TabStationToo problems to PCSX2.

## Status

Early and experimental.

- Games boot from disc images (uncompressed `.iso`, `.bin` or `.img`), read straight from your
  file. One game has been tested so far, Dragon Ball Z: Budokai Tenkaichi 3 (PAL): it runs from
  its language menu through the title and menus into a battle, driven from the keyboard, and its
  save on memory card slot 1 is kept in the browser and loaded again after a reload.
- The BIOS still boots on its own: the start-up animation, the console's menus and its settings.
- Speed: the emulated CPU is the limit, because a browser cannot run PCSX2's faster recompilers
  (see below). Measured in that game in Chrome on a Ryzen 7 PRO 7840U laptop, with other jobs
  running: title screen and logos 100 % of full speed, menus 60–100 %, character select 60–95 %,
  movies 20–70 %, and battles 10–16 % (5–8 of the PS2's 50 frames per second), which is not
  playable yet. Drawing is not what slows it down.
- Sound plays through Web Audio. Below full speed PCSX2 time-stretches it, so it plays slowed down
  rather than choppy.
- Not there yet: save states, gamepads, compressed disc images (CHD, CSO, GZ), and swapping discs
  while a game runs.
- Desktop Chrome and Edge are the targets. Other browsers may work; phones are not supported yet.

## How it works

- PCSX2's core is compiled with [Emscripten](https://emscripten.org) to WebAssembly. The
  emulated PS2 processors run on PCSX2's interpreters (its recompilers emit x86 or ARM code,
  which a browser cannot run), on threads backed by `SharedArrayBuffer`.
- The PS2's graphics are drawn by PCSX2's software renderer and shown in a `<canvas>`. A WebGPU
  renderer is planned.
- The page (`pcsx2-wasm/web/`) is plain HTML, CSS and JavaScript with no build step and no
  third-party requests. The BIOS you pick stays in your browser (IndexedDB).
- A disc image is never copied: a worker reads the parts the game asks for from your file and
  hands them to the emulator through shared memory
  ([docs/web-port/DISC-DESIGN.md](docs/web-port/DISC-DESIGN.md)). The memory card is a file the
  page keeps in IndexedDB.

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
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF \
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

Open http://127.0.0.1:8080/ and choose your BIOS file. Then press **Choose a disc…** (or drop a
disc image on the page) and press Boot; without a disc, Boot opens the console's own menus. Click
the screen to give it the keyboard, and press Esc to take the keyboard back. The Keys panel shows
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
| `pcsx2-wasm/` | The browser frontend: `Main.cpp` (the emulator's entry point and the API the page uses) and `web/` (the page) |
| `pcsx2-headless/` | A windowless runner for Node.js, used to test and benchmark without a browser |
| `common/Emscripten/`, `pcsx2/wasm/`, `pcsx2/CDVD/Emscripten/` | WebAssembly platform code for PCSX2's core (`CDVD/Emscripten/` reads discs through the page's disc worker) |
| `tools/wasm/` | Dependency build script, the dev server, and the scripts that assemble and smoke-test the deployed site |
| `docs/web-port/` | Porting plan and interface design |
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
