# TabStationToo

TabStationToo is a browser frontend for the WebAssembly build of PCSX2 (the `pcsx2-wasm`
target). It is built on PCSX2 (GPL-3.0) and is not affiliated with Sony or the PCSX2 project.
You pick your own PS2 BIOS and a folder of disc images of your own games, put a disc into the PS2
in the disc library, and the game (or the BIOS menu) renders in a `<canvas>`, driven from the
keyboard or a gamepad. The EE, IOP, VU0 and VU1 run on WebAssembly JITs that compile the PS2's
code at run time (`../pcsx2/wasm/`). Where the browser offers WebGPU, the GS draws with PCSX2's
hardware renderer on the GPU and posts each frame to the page as an `ImageBitmap` (the WebGPU
device, `docs/web-port/WEBGPU-DESIGN.md`); otherwise it uses the software renderer and its output
is copied to the page as plain RGBA pixels and drawn on a 2D canvas. Sound plays through a Web Audio
worklet. Memory cards plug into both slots from a library of named cards kept in the browser or in
`.ps2` files on disk. Save states (STATE on the remote, F1 and F3) are kept per game in the browser.

- `Main.cpp`: the frontend (Host callbacks, settings, CPU thread loop, `pcsx2web_*` exports)
- `web/`: the page (`index.html`, `app.js`, `disc-worker.js`, `audio-tap.js`, `style.css`,
  `icons.svg`, `favicon.svg`, `fonts/`, `icons/`, and `isolation.js` with `coi-serviceworker.js`
  for hosts without isolation headers), copied next to the build output. The design is
  `docs/web-port/UI-SPEC.md` (§10 for discs, §11 for sound, §14 for memory cards).
- `../pcsx2/CDVD/Emscripten/WebDisc.*`, `WebDiscFileReader.*`: the emulator's side of the disc
  reader; `web/disc-worker.js` is the page's side (`docs/web-port/DISC-DESIGN.md`).
- `../tools/wasm/serve.py`: local dev server with the headers the page needs
- `../tools/wasm/assemble-site.py`, `../tools/wasm/smoke-test.mjs`, `../.github/workflows/pages.yml`:
  the deployed site (see [Deploying](#deploying))

## Build

Needs Emscripten 6.0.12 and the wasm dependencies (built once):

```sh
source ~/emsdk/emsdk_env.sh
tools/wasm/build-dependencies-wasm.sh deps-wasm        # once; output in deps-wasm/

emcmake cmake -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/deps-wasm" \
  -DENABLE_GSRUNNER=OFF -DENABLE_TESTS=OFF
ninja -C build-wasm pcsx2-wasm
```

Use `-DCMAKE_BUILD_TYPE=Devel` (for example in `build-wasm-dbg`) for a build with assertions.
The target links with `-sAUDIO_WORKLET -sWASM_WORKERS` (sound, below): the module's JS is also
loaded into the AudioContext's worklet scope, so it must never touch `window` or `document` when it
loads (keep page code in `app.js`).
The target writes `pcsx2-wasm.js`, `pcsx2-wasm.wasm` and `pcsx2-wasm.data` (the resources) to
`<build>/bin/`, and copies `web/` there too, so `<build>/bin/` is the web root.

## Serve

The emulator's threads share memory (`SharedArrayBuffer`), which browsers only allow on a
cross-origin isolated page served from localhost or HTTPS with
`Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`.
Opening `index.html` from disk does not work. Use the dev server:

```sh
python3 tools/wasm/serve.py                        # serves build-wasm/bin on http://127.0.0.1:8080/
python3 tools/wasm/serve.py --port 9000 build-wasm-dbg/bin
```

It binds to 127.0.0.1 only, sends the isolation headers and `Cache-Control: no-store` on every
response, and never lists directories or serves anything outside the web root. `--no-isolation`
leaves the isolation headers out and `--max-age 600` lets the browser cache files for 10 minutes,
which together behave like GitHub Pages (see [Deploying](#deploying)). For the same
reason the page loads nothing from other origins: no CDN, and same-origin, self-hosted fonts
(Archivo and Chivo Mono, OFL 1.1, in `web/fonts/`; Phosphor icons, MIT, in `web/icons.svg`).

## Use

1. Open the URL. The emulator (about 14 MB) starts downloading at once and the disc library opens
   ([Disc library](#disc-library)). Its BIOS tile asks for your BIOS: press **Choose file** or drop
   the file on the page (usually a 4 MB `.bin`, for example `SCPH-39001.bin`). Its Games tile then
   takes a folder of disc images, and putting a disc into the PS2 boots it. On touch-only devices
   the download waits until you pick a BIOS.
2. Without the library (close it with Esc), the slate under it offers **Choose a disc…**; a disc image
   dropped on the page works too:
   an `.iso`, `.bin` or `.img`, or a compressed `.chd`, `.cso`, `.zso` or `.gz`, made from a disc you own.
   The page checks that a flat file holds an ISO 9660 volume where PCSX2 looks for one, and that a
   compressed file starts with its format's header, and refuses anything else (`.dump` block dumps get
   their own message). PCSX2's own readers decompress them, through `WebDisc::OpenCFile()` (a `FILE*`
   over the disc worker with a cache of 128 KiB windows). A `.gz` is scanned whole at every boot to
   build its index, so it starts slowly; CHD and CSO start at once. The file is not copied or
   uploaded: the disc worker reads the parts the game asks for, while it runs. In Chrome and Edge
   the picker (or a drop) also gives the page a handle to the file, kept in IndexedDB: after a
   reload the title card offers **Open it again**, which asks the browser for permission and opens
   the disc with one press. Other browsers need the file picked again. The disc menu under the
   screen (the disc's title) has **Choose another disc…** and **Remove the disc**. While the PS2 runs
   they swap or eject the disc without stopping it, for games on several discs: the page asks the
   emulator to eject (`pcsx2web_change_disc("")`, PCSX2's `ChangeDisc` to no disc), opens the new file
   in the disc worker, then inserts it (`pcsx2web_change_disc("/disc/<name>")`). Ejecting first means
   nothing reads the old disc once the worker has replaced it; if the new file can't be opened, the
   worker still serves the old one and it goes back in. A disc dropped on the page while a game runs,
   and the console's empty tray, swap the same way. `window.PCSX2WebDisc.load(url)` and `.remove()` do
   it from automated runs.
3. Press **Boot** (or Enter while the screen has focus). If the emulator is still downloading, the
   boot is queued; press again to cancel. With a disc, PCSX2 fast-boots it as its desktop version
   does (the BIOS intro is skipped) and the game's title, from PCSX2's game database, appears in the
   disc menu and the tab's title. Without a disc, the BIOS menus appear after a few seconds. A file
   that is not a PS2 BIOS (no ROM directory with a ROMVER entry, as PCSX2's own BIOS list requires)
   is refused before the VM starts, and the slate shows the reason from the log.
4. When the picture appears, the keyboard goes to the PS2 while the screen has focus (the amber
   frame line and the square under the screen's corner show it):

   | PS2 | Key | PS2 | Key |
   | --- | --- | --- | --- |
   | D-pad | arrow keys | Start | Enter |
   | Cross | K | Select | Backspace |
   | Circle | L | L1 / R1 | Q / E |
   | Square | J | L2 / R2 | 1 / 3 |
   | Triangle | I | L3 / R3 | 2 / 4 |
   | Left stick | W A S D | Right stick | T F G H |

   Keys are physical positions: on AZERTY the left stick is Z Q S D, and the **Keys** drawer shows
   the letters of your layout. **Escape** releases the keyboard (it never reaches the PS2); click
   the screen, or press Enter on "Click the screen to play", to take it back. Tab (which moves to
   that button and never reaches the PS2), opening the BIOS menu or About, leaving the window or
   hiding the tab also release it. Clicking Reset or the remote's buttons keeps the keyboard
   on the PS2. Browser shortcuts (Ctrl/Cmd/Alt combinations, F5, F11, F12) keep working. The controls fade after 2.5 s without
   pointer activity; **Keep controls visible** in the Keys drawer turns that off.
   **Gamepads** play too: a gamepad with the browser's standard layout (Xbox, DualShock, DualSense,
   Switch Pro and most others; press a button once so the browser lists it) plays by position
   (bottom face button = Cross), with analog sticks and triggers, and the Guide/Home button as the
   PS2's Analog button. It plays while the PS2 runs and the page has focus, whatever the keyboard's
   state. In the **Keys** drawer, the **Gamepad** tab shows a live picture of each gamepad and lets
   each device (the keyboard on its own tab, every gamepad) play as Player 1 or Player 2; two
   gamepads can be swapped. The choices are remembered in this browser. A gamepad that the browser
   does not lay out in the standard way (an 8BitDo in D mode on a Mac, for one) shows **Map controls**
   in that tab: press each control when asked and the mapping is kept in this browser.
5. Under the screen, the console's front (UI-SPEC.md §16): **Reset** stops the VM (then **Boot
   again**); **Eject** opens the disc library; the tray opens the disc menu and the plate beside
   the memory card slots the BIOS menu (where the BIOS is stored, **Choose another file…** and
   **Forget this BIOS**); the memory card slots open their cards' menus, and the controller ports
   plug and unplug Player 1 and Player 2. The remote beside it has **VOL** − and + and **MUTE**
   (M mutes and unmutes from the screen too; both are remembered in this browser), **WIDE**
   (Fullscreen; Alt+Enter, or a double-click on the picture), **PICTURE** (Smooth, Crisp or CRT),
   **MENU** (Graphics: renderer and resolution), **CLOCK** (Underclock, per game), **INFO** (the
   Log, `` ` ``, ² on AZERTY) and, with `?threads=1`, **REC** (the Recorder). `?` opens the Keys
   drawer. Under the console, the two controllers show what plays each control; click one to change
   it. The single-key shortcuts only work while focus is on the screen or the controls under it.

Use only a BIOS dumped from a console you own. The file never leaves the browser: it is written
into the emulator's in-memory file system (`/bios/<name>`) and remembered in the site's IndexedDB
so you do not have to pick it again. **Forget this BIOS** deletes it.

### Memory cards

The two slots, **Card 1** and **Card 2** in the controls, hold cards from a library the page keeps
(UI-SPEC §14). A card is an 8 MB `.ps2` file, the format desktop PCSX2 uses. The library is the
IndexedDB database `pcsx2-web-cards`: `cards` holds each card's name and, for a card linked to a
file on disk, its `FileSystemFileHandle`; `bytes` holds each card's 8 MB image; `meta` says which
card is in which slot and which was last in each. Only the cards in the slots reach the emulator: the
page writes each to `/memcards/<card id>.ps2` (MEMFS) and names it with `pcsx2web_memcard(slot,
file, id)`. The CPU thread applies that at the next vsync (or at the next boot): PCSX2 ejects the
card for 60 frames (`AutoEject`) and reopens the slot, so a game sees a card come and go, and the id
then shows in `pcsx2web_memcard_status()`, with a flag while PCSX2 counts the card busy (300 vsyncs
after a write). An unplug waits for that flag to clear, so a save is never cut short.

PCSX2 keeps a card file open and writes it in place (unbuffered on wasm), so the page copies the cards
to IndexedDB itself: 1.5 s after the game's last write, at once when the VM stops (Stop closes the
cards), when a card is unplugged, and when the page is hidden. It counts writes (`FS.write` on a card
file) to know what is unsaved. A linked card is then written to its file (`createWritable`, the whole
8 MB). A copy that fails is tried again after 2, 5 and 15 s; then the page stops copying until the
game writes again, and says that saves last for the visit. The copy on hiding or leaving does not
wait for that pause, and its failure does not count toward giving up. The copy is asynchronous and
cannot finish while the page unloads, so from the first card write until the copy is done the page
asks before it is closed or reloaded (`beforeunload`).

### Pipeline lists

The hardware renderer on WebGPU needs a render pipeline for each combination of TFX shader and
state a game draws with, and the browser takes from a few to tens of milliseconds to create one.
A draw whose pipeline does not exist yet starts creating it in the background
(`createRenderPipelineAsync`), waits for it at most 3 ms (and at most 4 ms in all for one vsync),
and is skipped if it is still not ready; the pipeline is collected at a later present and the
game's later draws use it. So a new effect may be missing for its first frames instead of the game
stalling. `?pipelines=wait` restores waiting to the end (the test switch for frames identical to
a run without skips). The log's line `WebGPU HW: ... draws skipped for a pipeline not ready yet`
counts the skips every 600 presents. The recorder's GPU pipeline columns (`pcsx2web_perf_ring()`)
count the pipelines draws started creating and the time draws waited for them; prewarmed
pipelines (below) are not counted.

The page also keeps, per game, the list of pipelines its draws asked for, and creates them in the
background at the game's next boot, as soon as the game is known, so that most are ready before
the game needs them. The renderer logs the keys of the pipelines its draws ask for
(`pcsx2web_tfx_log()`); every 2 s the page copies new ones, and stores the list in the IndexedDB
database `pcsx2-web-pipelines` (store `tfx`, keyed by the game's serial: `{version, keys, savedAt}`,
`keys` 36 bytes per pipeline, at most 4096) every 15 s while it grows, when the VM stops and when
the page is hidden. The list is this session's keys in the order the game first asked for them,
then the stored ones it did not use. When the game's serial is known (`pcsx2web_game()`), the page
writes the stored list into the prewarm slot (`pcsx2web_tfx_prewarm()`); the GS thread takes it at
its next present and starts four at a time (eight in creation at once). `version` identifies what a
key means (the key's layout, `tfx.wgsl`, the shader macros each selector bit sets, the texture
formats); the GS thread ignores a list made by a build whose keys meant something else, and the next
save replaces it. Only disc boots with a serial keep a list.

### Code lists

The EE JIT compiles game code once a block has run often enough, within 4 ms of compiling per vsync
(`pcsx2/wasm/JitHeat.h`); until then the interpreter runs it. Building a module, about three quarters
of compiling it, runs on a thread of its own: the EE thread copies the code it reads, goes on
interpreting, and only compiles the finished module with the browser's engine (`pcsx2/wasm/EEJit.cpp`,
`CompileJob`). The page also keeps, per game, the list of blocks the JIT compiled, so that the next
time the game runs, they compile before the game reaches them: the build thread builds the list's
modules whenever it has nothing else to build (at most 16 waiting), and each waits until the game
reaches one of its blocks, which then switches it in at once, or until the frame limiter has time left
to compile it with the engine (loading screens, light scenes). Without the build thread
(`-eejit-bg 0`), listed blocks compile on the EE thread in the limiter's spare time only. The JIT logs each block it compiles, its PC and a hash of its
first words (`pcsx2web_ee_code_log()`); every 2 s the page copies new ones and stores the list in the
IndexedDB database `pcsx2-web-code` (store `ee`, keyed by the game's serial:
`{version, entries, savedAt}`, `entries` two 32-bit words per block, at most 32768) every 15 s while it
grows and when the VM stops. When the game's serial is known, the page writes the stored list into the
EE thread's slot (`pcsx2web_ee_code_list()`), which takes it at its next vsync. A listed block is
compiled ahead only when its code is in memory with the same hash; compiling earlier changes when
code is compiled, never what it does. `?codelist=0` keeps the lists without handing them over.

A linked card is read from its file when it is plugged in (the file wins over the browser's copy).
After a reload it stays plugged in only if the browser still allows the page to write the file;
otherwise it comes back unplugged, and choosing it in the slot's menu asks for permission. A file that
cannot be written any more unlinks the card: it goes on from the browser's copy. Without the File
System Access API (Firefox, Safari), cards live in the browser, and **Open a card file…** and **Save
a copy to disk…** move them in and out.

Only one tab keeps the cards: the page holds the Web Lock `tabstationtoo-memcards` for as long as it
is open (waiting up to 2 s for it at start-up, for the page a reload replaces). A second tab loads the
cards in the slots but never writes them back, and its card menus are disabled.

Earlier versions kept slot 1's card in Emscripten's IDBFS (database `/memcards`). On the first visit
with this version the page moves that card into the library as "Memory card 1", plugged into Card 1,
then deletes the old database. The page no longer links IDBFS: one copy per card, written by the page,
is simpler than two file systems that could disagree.

The **Log** drawer shows PCSX2's log (the last 2000 lines), with **Copy** for bug reports.

### Automated runs

```sh
python3 tools/wasm/serve.py --bios-dir build-wasm/data/PCSX2/bios
# then open http://127.0.0.1:8080/index.html?bios=dev-bios/scph39001.bin
```

`--bios-dir` serves the files directly inside that directory, read-only, under `/dev-bios/`.
`?bios=<url>` (same origin only) fetches the BIOS and queues a boot for when the runtime is ready
(Boot cancels it), without saving the BIOS to IndexedDB. `?console=1` also mirrors the log to the
browser console. A test can poll `window.PCSX2Web.stats()` (VM state, page state as `view`, speed,
VPS, frames presented and dropped, frame size, the display path, the vsync of the frame on screen,
WebGPU frames shown and posted, the devices whose frames arrived, the post-to-display latency) and
read `window.PCSX2Web.logText()`.

Switches for comparisons and tests (all optional):

| Query | Effect |
| --- | --- |
| `?gpu=0` | No WebGPU: the GS presents through its in-memory device and the page draws on a 2D canvas |
| `?renderer=sw` | Draws with PCSX2's software renderer instead of the hardware renderer on the WebGPU device (the default, `?renderer=hw`). Without a WebGPU device the GS uses the software renderer |
| `?set=Section/Key=Value` | Overrides one setting after the defaults (repeatable), e.g. `?set=EmuCore/GS/deinterlace_mode=2` |
| `?surface=WxH` | Presents at a fixed size on both paths (e.g. `640x448` with `?set=EmuCore/GS/AspectRatio=Stretch` for 1:1) |
| `?threads=1` | Adds how busy the EE, GS and VU1 threads are to the speed readout (`pcsx2web_thread_usage()`), to see which one limits a game without the developer tools, which slow WebAssembly down a lot while open; and a Recorder button: it records every vsync (its speed, each thread's load, JIT compiling on the EE and VU1 threads and on the EE JIT's build thread, EE modules compiled and compiled ahead, GPU pipeline compiling, from `pcsx2web_perf_ring()`) until stopped, then charts it, crops it to an interval and copies it as text or downloads it as CSV. The Recorder also has Save state, which snapshots the whole emulated machine and downloads it as a `.p2s` file; Load state…, which loads a `.p2s` file you pick into the running game; and Reload state, which goes back to the last state saved or loaded, to replay the same moment while measuring |
| `?tune-cables` | Shows the cable tuner: sliders for how the controllers' cables lie (`cable-worker.js`, `CABLE_DEFAULTS`: each cable's seed, slack, stiffness, coil memory, drop, settling and smoothing), both cables shown even unplugged; the settings are kept in this browser and Copy settings copies them as JSON, to become the defaults |
| `?capture=N,M,...` | Keeps a copy of the first frame presented at vsync N or later, read with `window.PCSX2Web.capture(N)` |
| `?eejit=0` | Runs the EE (main CPU) interpreter instead of the EE JIT, which compiles EE code to WebAssembly at run time |
| `?iopjit=0` | Runs the IOP (I/O processor) interpreter instead of the IOP JIT, which compiles IOP code to WebAssembly at run time |
| `?vu1jit=0` | Runs the VU1 (vector unit 1) interpreter instead of the VU1 JIT, which compiles VU1 micro programs to WebAssembly at run time |
| `?gsidle=block` | Keeps the GS thread's idle wait blocking with WebGPU, leaving only the device's bounded event-loop turn |
| `?pipelines=wait` | The hardware renderer waits for every new TFX pipeline to the end, as before pipelines were created in the background, so no draw is skipped (for pixel-exact comparisons; see Pipeline lists) |
| `?unlimited=1` | Boots with the frame limiter off (as `pcsx2-headless -unlimited`), to measure speed above 100 % |
| `?elf=<url>` | Fetches a PS2 program (same origin only) and boots it instead of a disc or the BIOS menu, with the `?bios=` BIOS and fast boot (see below) |
| `?state=<url>` | Fetches a save state (same origin only; for example `dev-bios/<file>.p2s` with a state file in `serve.py --bios-dir`'s folder), which the next disc boot loads once the game started, and Reload state too. The state must come from the same game |

`tools/wasm/pad-test.mjs` tests the players (the keyboard and gamepads going to port 1 or 2): it
plays a mocked `navigator.getGamepads()` and the keyboard in headless Chrome and reads each port's
pad through `pcsx2web_test_pad_state`:

```sh
node tools/wasm/pad-test.mjs --bios-dir DIR --bios FILE   # [--root build-wasm/bin] [--chrome PATH]
```

In Firefox (and Zen), quick gamepad taps can be lost unless `dom.gamepad.non_standard_events.enabled` is set to true in `about:config`: release builds hand the page some taps' press and release together, and only those (non-standard) events say which button it was. The Gamepad tab says so.

In Firefox (and Zen) `transferToImageBitmap()` drops a WebGPU canvas's configuration, so the GS configures its canvas again after every frame it posts (`webgpu_host.js`, `tst_post_frame`); without that the WebGPU display stopped after its first frame.

Frames are identified by the vsync they show, counted from boot, not by how many were presented:
PCSX2 skips presenting duplicate frames (`SkipDuplicateFrames`) based on a wall-clock measure, so
the count depends on timing while the vsync number does not. `pcsx2web_test_snapshot(n)` asks
for a screenshot (`GSRenderer::SaveSnapshotToMemory`) at vsync n, written to
`/snapshots/frame-n.png`; `pcsx2web_test_lose_device()` makes the WebGPU device destroy itself at
its next present, as on a device loss. `tools/wasm/run-app-headless.mjs` drives all of this in
headless Chrome on the machine's GPU (see its header for the options).

### WebGPU and the fallback

The GS thread owns the WebGPU device; the module is linked with `-sJSPI` so that thread can wait
for WebGPU (device creation, readbacks) and so its idle wait lets the worker's event loop run.
WebGPU is used only where the browser has both WebGPU and JSPI. In a browser without JSPI,
`jspi-fallback.js` (linked with `--pre-js`) stands in for `WebAssembly.Suspending` and
`WebAssembly.promising` so the module still starts, and the page uses the 2D path.
The page receives a `MessagePort` once (`Module.tstFramePort`) and shows every `ImageBitmap`
posted on it with a `bitmaprenderer` context, at the canvas's backing size, which it reports with
`pcsx2web_surface_size()` (the device presents at most 4096 pixels on a side, and never more than
the adapter's largest texture; the page scales a smaller frame to the canvas). In Firefox, and on
the 2D path, the page reports at most 512 lines instead (keeping the canvas's shape) and CSS scales
the frame up: there each presented pixel costs a GPU readback (Firefox's `transferToImageBitmap()`
on a WebGPU canvas: 9.6 ms a frame at 1960x1470 in Zen 1.22 on an M1 Max, 2.9 ms at 640x512) or CPU
time (the 2D path's scaling). **Crisp** switches
the device between nearest and bilinear filtering (`pcsx2web_set_crisp()`); on the 2D path it
switches the canvas's CSS scaling instead.

On a device loss (a GPU-process crash, for example) the GS recreates the device and carries on.
If that fails twice in a row (no new device, or the device lost again within 15 seconds of each
of two recoveries), the GS gives WebGPU up for the page: its frames come through the 2D path, the
page swaps in a fresh canvas, and the status line says so when WebGPU frames had been shown. If
no device can be created at boot (four adapter requests over 0.75 s), only that boot uses the 2D
path; the next boot tries WebGPU again. Chrome itself may refuse WebGPU adapters for a while after
GPU-process crashes, which then counts as a failed recovery. A wait for the GPU that times out (a
readback after 2 s, a pipeline after 10 s) is handled as a device loss. Frames drawn between a
GPU-process crash and the moment Chrome reports the loss (0.2–0.6 s) are blank, and a frame the
page already shows does not survive the crash either (its memory was in the crashed process), so
the canvas stays blank until the recovered device's first frame.

Sound in automated runs (never let a test play through speakers: headless Chrome takes
`--mute-audio`, and the page takes `?mute=1`):

- `?bios=` boots without a user gesture, so the browser holds the sound until the first key press or
  click (Chrome's `--autoplay-policy=no-user-gesture-required` lifts that for tests).
- `?mute=1` forces the output gain to 0 for the visit; the stored volume and mute are untouched.
- `?audio=null` runs the emulator with PCSX2's Null audio backend (passed to `main()` as
  `-audio-null` through `Module.arguments`) and makes no AudioContext: a baseline for cost
  measurements.
- `?audiotap=SECONDS` routes the worklet's output through `audio-tap.js`, a pass-through
  AudioWorkletProcessor that records it before the gain, up to SECONDS in all.
  `window.PCSX2WebAudio.takeCapture()` returns what was recorded since the last call (interleaved
  float32 stereo, base64, with the context frame it starts at) and `interrupt()` suspends the
  context as a browser interruption would.
- `?audiorate=HZ` makes the AudioContext at HZ instead of 48000, which exercises the worklet's
  resampling (as PS1 mode's 44.1 kHz stream does in a 48 kHz context).
- `window.PCSX2WebAudio.stats()` reports the context (state, rate, `baseLatency`, `outputLatency`),
  volume, mute, gain, the Sound control's state, and the audio block (below).

Discs:

```sh
python3 tools/wasm/serve.py --bios-dir build-wasm/data/PCSX2/bios --disc /path/to/game.iso
# it prints the page's links, for example
# http://127.0.0.1:8080/index.html?bios=dev-bios/<bios>&disc=/dev-disc/<file name, encoded twice>
```

`--disc FILE` serves exactly that one file, read-only and with byte ranges, under `/dev-disc/`.
`?disc=<url>` (same origin only, served with byte ranges) opens the disc in the disc worker; with
`?bios=` the queued boot waits for it and then boots the disc. The disc is not remembered. Like
`?bios=`, the value is a URL inside a query string, so it is percent-encoded once more
(`encodeURIComponent` of the URL): the page decodes the query once and then reads the value as a
URL. A name with only letters, digits, spaces, `-`, `,`, `.` and parentheses also works with one
encoding, but `#`, `?`, `&`, `+` and `%` need both.
`?fastboot=0` boots discs through the BIOS intro instead of fast boot. `window.PCSX2WebDisc.stats()`
reports the disc (name, size, source, whether it is open), the disc reader's state and read counts,
the game's title and serial as PCSX2 reported them, and the memory cards (library, slots, unsaved writes).

PS2 programs (developer runs, for example the benchmark in `tools/wasm/bench/`):

```sh
python3 tools/wasm/serve.py --bios-dir DIR --elf tools/wasm/bench/bench.elf
# it prints the page's link, for example
# http://127.0.0.1:8080/index.html?bios=dev-bios/<bios>&elf=dev-elf/bench.elf
```

`--elf FILE` serves exactly that one file, read-only, under `/dev-elf/`. `?elf=<url>` (encoded like
`?disc=`) fetches it; the queued `?bios=` boot waits for it, writes it to `/elf/<name>` in the
module's filesystem and boots it with `pcsx2web_boot_elf()`. What the program prints reaches the log
(and, with `?console=1`, the browser console). `node tools/wasm/bench/run-bench.mjs` drives such a
run in Zen or Chrome and reports the speed (see its header). `pcsx2-headless -elf FILE` does the same
without a browser.

The three JITs are on by default: `?eejit=0`, `?iopjit=0` and `?vu1jit=0` switch them off one at a time (the VU0
micro-mode JIT stays off). VU1 JIT testing (`docs/web-port/VU1-JIT-DESIGN.md`; not a user setting):

- `?vu1jit=0` boots with the VU1 interpreter (`EmuCore/CPU/Recompiler/EnableVU1=false`).
- `?vu1jit=1` boots with the VU1 JIT, as by default, and its counters (self-test,
  programs compiled, instructions, of them on the interpreter) go to the browser console every 10 s.
- `?vu1jit=fuzz:SEED:COUNT[:timing|jit|both]` runs the JIT's fuzzer on a worker, without a BIOS or
  a VM (boots are refused while it runs), and logs its summary: `timing` checks the timing model at
  every instruction of COUNT random programs, `jit` runs each one on the interpreter and on the JIT
  and compares them, `both` does both.
- `window.PCSX2Web.vu1jit()` returns the mode, the counters and the fuzzer's status.

The headless runner has the same switches and more: `-eejit 0`, `-iopjit 0`, `-set EmuCore/CPU/Recompiler/EnableVU1=false`,
`-vu1jit-fuzz SEED:COUNT[:MODE]`, `-vu1jit-oracle` (the timing model checked at every VU1
instruction of a real run), `-vu1jit-lockstep` (every VU1 program run on the JIT and on the
interpreter and compared), `-vu1jit-stats FILE` (per-vsync VU1 statistics as CSV) and `-mtvu-sync`
(the EE waits for the VU thread at each check, so whole runs repeat exactly). The EE and IOP JITs
compile a block once it has run 128 times, within 4 ms of compiling per vsync (`pcsx2/wasm/JitHeat.h`):
`-eejit-hot N[,M[,B]]`, `-iopjit-hot N[,M[,B]]` and `-jit-budget US` change that, and
`-jit-compile-stats VSYNCS` prints their compile time, total and worst vsync, every VSYNCS vsyncs.

### Save states

STATE on the remote (UI-SPEC.md §19) saves and loads PCSX2 save states (`.p2s`) of the running game, with
F1 (save) and F3 (load the newest) while the screen or its controls have focus. They are kept per game serial
(the console's menus have their own key) in IndexedDB, `pcsx2-web-states`, at most 10 per game, each with a
128 × 96 JPEG of the picture at that moment (the next WebGPU frame, or the 2D canvas). The emulator writes and
reads them in `/states` through `pcsx2web_save_state()` and `pcsx2web_load_state()`, the calls the Recorder's
developer states use; the two share one busy lock. PCSX2 refuses to load while a memory card was written in
the last few seconds, so the page checks the cards first and says so. "Open a state file…" loads a `.p2s`
into the running game and keeps it with that game's states (a file does not say which game it is from).

### Disc library

The page's home is a dialog with a shelf of the discs in a folder and a PS2 to put them in
(UI-SPEC.md §15): it opens by itself once a BIOS is ready, and "Disc library…" (on the slate and in
the disc menu) opens it again. Putting one in chooses it and queues its boot at
once; the insert plays while the PS2 starts, and the dialog stays dark until the game shows a
picture, so the boot is never seen. The component is `library/disc-library.js` (with
`disc-library.css`); `library/player-library.js` wires it to `app.js` through
`window.PCSX2WebLibrary`. `?clean=1` makes every insert play the Easter egg (a disc that has to be
cleaned before it reads), which otherwise comes one time in ten.

A gamepad plays the shelf: the D-pad or the left stick moves along it and Cross puts the disc in front in.
`app.js` reads the gamepads (`libraryPadFrame()`) and hands the presses to the library through
`PCSX2WebLibrary.onPad()`; nothing a gamepad does while the dialog is open reaches the PS2.

- The console is a photo (`library/ps2.webp`, Evan-Amos, public domain; `library/ps2-photo.txt`).
  The tray is the photo's own front panel, cut out and slid along the console's depth; the plate
  and the disc behind it are drawn on the tray's plane with an affine transform measured from the
  photo (`TRAY` in `disc-library.js`).
- `library/disc-info.js` reads the start of each file (the ISO 9660 volume at the layouts PCSX2
  detects, then `SYSTEM.CNF`) for the serial; it also runs in Node. Titles come from
  `game-titles.tsv`, an index of PCSX2's game database the build writes next to the page
  (`tools/wasm/make-game-titles.py`). Served straight from `web/`, without it, titles come from the
  file names.
- Disc images carry no box art. A disc is printed with an image from the same folder named like
  PCSX2's covers (`SLUS-20946.jpg`, `.png` or `.webp`, or the game's title, or the disc's file name),
  else with the game's cover from the [ps2-covers](https://github.com/xlenore/ps2-covers) collection
  on GitHub, fetched by serial (the one request these pages make to another origin; GitHub sends
  `Cross-Origin-Resource-Policy: cross-origin`, so it loads in an isolated page), else with a label
  `library/disc-art.js` draws from the title and serial.

## UI assets

The page self-hosts everything it loads (it is cross-origin isolated and makes no requests to
other origins). Fonts and icons come from pinned packages managed with pnpm in `assets/`:

| Asset | Package | Licence | In `web/` |
|---|---|---|---|
| Archivo (variable, Latin) | `@fontsource-variable/archivo` | SIL OFL 1.1 | `fonts/archivo.woff2`, `fonts/Archivo-OFL.txt` |
| Chivo Mono (variable, Latin) | `@fontsource-variable/chivo-mono` | SIL OFL 1.1 | `fonts/chivo-mono.woff2`, `fonts/ChivoMono-OFL.txt` |
| Phosphor icons (Regular) | `@phosphor-icons/core` | MIT | `icons.svg`, `icons/Phosphor-LICENSE.txt` |
| coi-serviceworker (verbatim) | `coi-serviceworker` | MIT | `coi-serviceworker.js`, `coi-serviceworker-LICENSE.txt` |

`icons.svg` is generated: the Phosphor icons listed in `assets/sync-assets.mjs` followed by the
in-house symbols in `assets/icons-local.svg` (face-button glyphs and the mark). To update an
asset, change its version in `assets/package.json`, then in `assets/`:

```sh
pnpm install
pnpm run sync
```

Commit the regenerated files in `web/` together with `package.json` and `pnpm-lock.yaml`.

## Deploying

The site is static. `.github/workflows/pages.yml` builds it on GitHub Actions and publishes it on
GitHub Pages; the same tools work by hand.

### What is published

```sh
python3 tools/wasm/assemble-site.py --out _site     # after building pcsx2-wasm in build-wasm
```

`assemble-site.py` copies every file of `web/` and `pcsx2-wasm.js`/`.wasm`/`.data` from
`build-wasm/bin/` into `_site/`, and nothing else: no headless or GS runner, no BIOS (the build's
`data/` directory holds one), no test files. It stops with the reasons when a page file differs
from `web/` (a stale build), when the `.data` package holds anything outside `/resources/`, when
any file looks like console or game data (a BIOS ROM directory, an ISO 9660 volume, BIOS, disc,
memory card or save state names and extensions), or when a file is over 100 MiB (GitHub's limit
for a push) or the site over 1 GB (GitHub Pages' limit). Today the site is 17 files, about 14 MB.

### Cross-origin isolation without headers

GitHub Pages cannot send `Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy`.
`index.html` therefore loads two scripts before anything else:

- `coi-serviceworker.js` ([coi-serviceworker](https://github.com/gzuidhof/coi-serviceworker) 0.1.7,
  MIT, copied verbatim by `assets/sync-assets.mjs`) registers itself as a service worker for the
  site's directory. The worker passes every request through to the network and adds the headers to
  the response. On the first visit the page reloads once so that the worker serves it; after that
  the worker is already in charge and nothing reloads. It caches nothing: the HTTP cache decides
  what is fresh, as without it.
- `isolation.js` configures it and tells `app.js` what is happening. `app.js` waits for the reload
  instead of reporting the page as unsupported, and loads nothing meanwhile. When the worker cannot
  help (no service workers in a private window, site data blocked, the reload did not isolate the
  page, or nothing happened within 15 s), the page shows "This site needs a service worker to run
  the emulator, and this browser blocked it. Private windows often do." and the Log gives the
  reason. A second reload within a minute is allowed (a reload can race the worker's activation);
  past that it stops, so it can never loop.

When the server sends the headers (`serve.py`), the page is isolated from the start and neither
script does anything: no worker is registered. A worker registered earlier for the same origin
(scheme, host and port), for example by a `serve.py --no-isolation` run, stays until the site data
is cleared; keep separate ports for the two.

The worker uses COEP `require-corp`, the policy `serve.py` sends, rather than `credentialless`:
the page loads nothing from other origins, so `require-corp` costs nothing, Safari only supports
`require-corp`, and coi-serviceworker keeps the `credentialless` choice in a variable that is lost
whenever the browser restarts the worker. A later service worker (offline use, caching) must be
merged with this one: a scope has one worker.

### Updates and caching

GitHub Pages sends `Cache-Control: max-age=600` on every file (and serves byte ranges), so a
browser may reuse any file for up to 10 minutes without asking. To keep the page, the emulator and
its workers from one deploy together, `assemble-site.py` stamps a build id (a hash of every file)
into `index.html`: `app.js`, `style.css`, `isolation.js` and `icons.svg` are loaded as
`?v=<build id>`, and `app.js` adds the same `?v=` to `pcsx2-wasm.js` (and so to the pthread
workers, which load the script's own URL), `pcsx2-wasm.wasm`, `pcsx2-wasm.data` and
`disc-worker.js`. GitHub Pages ignores the query; the browser's cache does not. After a deploy:

- a visit within 10 minutes of the previous one may still show the previous deploy, whole: its
  `index.html` asks for its own `?v=` files;
- a reload, or any visit once the 10 minutes are over, gets the new `index.html` and with it only
  new files (the new `?v=` has never been cached);
- the only mix left is a cached old `index.html` whose own files have been evicted from the cache
  meanwhile: they then come from the new deploy. Bumping the build id cannot prevent that; only
  file names with the hash in them could.

`coi-serviceworker.js` keeps its plain URL: a worker is identified by its script's URL, so a new
URL on every deploy would reinstall it and reload every visitor's page. The browser checks the
script for changes itself on navigations, bypassing the HTTP cache.

Everything the page keeps in the browser (the remembered BIOS, the memory cards, the last disc's
file handle, preferences) belongs to the site's origin. Moving the site to another address starts
from nothing there, so choose the final address before people use it. On a project site
(`<user>.github.io/TabStationToo/`) the origin is shared with the user's other project sites.

### The workflow

`.github/workflows/pages.yml` runs on pushes to `wasm` that change more than Markdown files or
`docs/`, and by hand (`gh workflow run pages.yml --ref wasm`). It runs only in
`TabStationToo/TabStationToo` and only for `wasm`: never on forks or on `master`. Its build job
(read-only token) installs Emscripten 6.0.12 from the official emsdk repository at a pinned commit,
builds the third-party libraries with `tools/wasm/build-dependencies-wasm.sh` (each cached; the
libraries by the script's hash), builds `pcsx2-wasm` through ccache (`EM_COMPILER_WRAPPER`, cached
per commit), fails on `signature mismatch`, assembles the site and runs `tools/wasm/smoke-test.mjs`
twice in headless Chrome: without the isolation headers (the service worker's path, as on GitHub
Pages) and with them. Each run must reach the BIOS intake with the runtime ready,
cross-origin isolated, with no console error, failed request or request to another origin. A newer
push cancels a build in progress; deploys queue and never overlap. Every action is pinned to a
commit SHA.

A third job deploys the site to this repository's GitHub Pages,
https://tabstationtoo.github.io/TabStationToo/ (Settings > Pages > Source: GitHub Actions; the
`github-pages` environment must allow `wasm`). It needs no secrets: `actions/deploy-pages` uses the
workflow's own token with `pages: write`. The smoke test runs locally too:

```sh
python3 tools/wasm/assemble-site.py --out _site --force
node tools/wasm/smoke-test.mjs --site _site            # like GitHub Pages
node tools/wasm/smoke-test.mjs --site _site --headers  # like serve.py
```

To try a deploy by hand, serve `_site` like GitHub Pages and open it in a browser:

```sh
python3 tools/wasm/serve.py --no-isolation --max-age 600 --port 8090 _site
```

## How the page talks to the emulator

`main()` runs on a worker (`-sPROXY_TO_PTHREAD`), so the browser main thread only runs `app.js`.
It calls exports that never block (atomics and a lock-free key queue only):

| Export | Use |
| --- | --- |
| `pcsx2web_boot(name)` | boot `/bios/<name>` with no disc; 0 if accepted, -1 otherwise |
| `pcsx2web_boot_disc(name, path, flags)` | boot the disc at `path` (`/disc/<file>`, see below) with `/bios/<name>`; `flags` 1 = full boot (BIOS intro), 0 = fast boot; 0 if accepted, -1 otherwise |
| `pcsx2web_boot_elf(name, path)` | developer runs (`?elf=`): boot the PS2 program at `path` (`/elf/<file>`) with `/bios/<name>`, no disc, fast boot; 0 if accepted, -1 otherwise |
| `pcsx2web_set_boot_state(path)` | developer runs (`?state=`): the save state (`/states/<file>`) the next boot loads, or `""` for none; only while no VM starts or runs |
| `pcsx2web_save_state(path)`, `pcsx2web_load_state(path)`, `pcsx2web_state_result()` | developer runs (Recorder): save the running VM's state to, or load it from, `/states/<file>` on the CPU thread; 0 if queued. The result's value, `(sequence << 1) \| ok`, changes when one finishes |
| `pcsx2web_disc_block()` | address of the disc request block the disc worker serves (`DISC-DESIGN.md` §4.1) |
| `pcsx2web_game()` | address of the running game's details: `seq, disc CRC, ELF CRC` (u32), `serial[32]`, `title[256]` (UTF-8); `seq` is odd while they change |
| `pcsx2web_stop()` | request shutdown |
| `pcsx2web_state()` | 0 waiting, 1 starting, 2 running, 3 stopped, 4 failed (polled every 500 ms) |
| `pcsx2web_speed()`, `pcsx2web_vps()` | speed in percent, vertical syncs per second |
| `pcsx2web_thread_usage(t)` | how busy the EE (0), GS (1) and VU1 (2) threads were, in percent: wall time not spent waiting for another thread (WebAssembly has no per-thread CPU clocks) |
| `pcsx2web_perf_ring()`, `pcsx2web_perf_written()` | the per-vsync recorder's ring (8192 records of 12 doubles: vsync, wall ms, cumulative busy ms of the EE, GS and VU1 threads, cumulative JIT compile ms on the EE and VU1 threads, cumulative GPU pipeline ms and count, vsync rate, cumulative ms the EE JIT's build thread spent building modules, cumulative EE modules installed and those built or compiled ahead from the code list) and how many records were written |
| `pcsx2web_key_code(name)`, `pcsx2web_key(code, pressed)` | resolve host key names once, then queue key events |
| `pcsx2web_pad_input(name)`, `pcsx2web_pad(port, input, value)` | resolve a pad input name ("Cross", "L2", "LUp", ...) once, then queue its state (0 to 1) for port 0 or 1; 1 if queued, 0 if the queue is full. The page routes the keyboard and the gamepads to a port (UI-SPEC §13); the core has no keyboard bindings for the pads |
| `pcsx2web_test_pad_state(port)` | test hook: the raw value (0 to 255) of each pad input of a port, four to a word, then a flags word (bit 0 connected, bit 1 analog light) |
| `pcsx2web_frame()` | address of the frame descriptor: `seq, width, height, pixels, vsync` (u32) |
| `pcsx2web_surface_size(w, h, scale)` | the display canvas's backing size and device pixel ratio (WebGPU presents at it) |
| `pcsx2web_set_crisp(on)` | nearest (1) or bilinear (0) presentation, applied at the next vsync |
| `pcsx2web_pad_vibration()` | address of two `u32` words, one per pad port: the game's motors as `large << 8 \| small` (0-255 each), updated on every pad poll (`InputManager::SetPadVibrationIntensity`) and zeroed on pause. The page plays them on that player's gamepads (`rumbleFrame()`); `PCSX2Web.stats().rumble` shows them |
| `pcsx2web_set_wheel(port, type)` | plugs one of PCSX2's emulated Logitech wheels into USB port 0 or 1 (0 Driving Force, 1 Driving Force Pro, 2 Driving Force Pro rev 11.02, 3 GT Force; -1 nothing), applied at the next vsync or boot. The pads stay as they are. The console's USB 1 and USB 2 sockets open the page's menu for it (`wheel.js`) |
| `pcsx2web_wheel(port, bind, value)` | queues one input of the emulated wheel on USB port 0 or 1 by its bind index (`usb_pad::ControlID`: 0/1 steering left/right, 2 throttle, 3 brake, 4-7 D-pad, 8 onwards buttons) and a value 0 to 1; 1 if queued, 0 if the queue is full |
| `pcsx2web_ffb_ring()` | address of the force feedback ring: words head, tail, dropped, size, then two words per command (the game's 7 raw bytes, the USB port in the 8th). The page takes the commands up to head, stores tail, and sends them to that port's real Logitech wheel with WebHID, holding back the mode-change, rev-light and exact-range extended commands (`F8 01/05/06/09/0A/10/11/12/81`), and the 200°/900° ones (`F8 02/03`) from wheels without them |
| `pcsx2web_set_upscale(n)` | the internal resolution (`EmuCore/GS/upscale_multiplier`): 1 (native, the default) or 2 (twice the PS2's lines), applied at the next vsync or boot; other values are ignored. The hardware renderer only (the software renderer ignores it); the GS clamps it to the adapter's largest texture. The page's Graphics menu offers Native and 2× |
| `pcsx2web_set_tv_shader(n)` | PCSX2's TV shader (`EmuCore/GS/TVShader`: 0 none, 1 scanlines, 2 diagonal, 3 triangular, 4 wave, 5 Lottes CRT, 6 4xRGSS, 7 NxAGSS), applied at the next vsync; the WebGPU device only (the 2D path's device has none). The page's CRT picture mode is 5 |
| `pcsx2web_set_ee_cycle_rate(rate)` | the Underclock (PCSX2's EE cycle rate): 0, or -1 to -3 for the EE at 75, 60 or 50 %, applied at the next vsync or boot; other values are ignored |
| `pcsx2web_vsync_count()` | vsyncs emulated since the module started |
| `pcsx2web_audio_start()` | start the sound worklet in `Module.pcsx2webAudioContext` (an AudioContext the page made); it calls `Module.pcsx2webAudioReady(node)` when the node exists, or with `null` if it failed; 0 if started, -1 if a start already ran or there is no context |
| `pcsx2web_audio()` | address of the audio block: 16 u32 words (see Sound) |
| `pcsx2web_tfx_log()` | address of the log of TFX pipeline keys the WebGPU HW renderer's draws asked for since its device started: `session, count, version lo, version hi` (u32), then `count` keys of 36 bytes; `session` changes when a new device starts a new log (see Pipeline lists) |
| `pcsx2web_ee_code_log()` | address of the EE JIT's code log since the VM started: session, count, version (two words), then count entries of PC and code hash |
| `pcsx2web_ee_code_list()` | address of the slot a stored code list is written into, the same layout with a state for its first word: 0 free, 1 the page is writing, 2 ready. The page claims it by compare-exchange (0 or 2 to 1), writes the entries, version and count, then stores 2; the EE thread takes it at its next vsync |
| `pcsx2web_tfx_prewarm()` | address of the prewarm slot, the same layout with a state for its first word: 0 free, 1 the page is writing, 2 ready, 3 the GS thread is taking it. The page claims it by compare-exchange (0 or 2 to 1), writes the keys, count and version, then stores 2 |

On the 2D path, each animation frame the page reads `seq` with `Atomics.load`; when it changed, it copies
`width * height * 4` bytes of RGBA from `pixels` into an `ImageData` (which cannot use shared
memory), re-reads `seq` and drops the copy if a newer frame may have overwritten it, then draws it.
Views on the wasm memory are recreated every frame because memory growth replaces the buffer.

Discs: at start-up (`Module.postRun`) the page starts `disc-worker.js` and attaches it to the
request block with the shared `WebAssembly.Memory`. When a disc is chosen, the worker opens the
`File` (or the URL); once it is open the page writes a 0-byte placeholder at `/disc/<file>`, so that
PCSX2's existence checks pass, and boots that path. `GetFileReader()` gives `/disc/` paths a
`WebDiscFileReader`, which reads 128 KiB chunks through the worker: the reading thread posts the
range into a slot and sleeps on it, and the worker copies the bytes straight into the wasm memory.
A read gives up after 10 s, and `pcsx2web_stop()` cancels reads in progress. A source that timed out
or cannot be reached is then marked unavailable in the worker: the next reads fail at once (PCSX2
retries a failed read up to 256 times) while the worker probes the source every 2 s, and reads go
to it again once it answers. A stalled source therefore holds the emulator once, for up to 10 s, and
the page says so. Before each disc boot the worker reads the start of the disc, so a disc that went
away is reported on the slate instead of hanging the boot. If the worker dies, the page waits until
it is really gone (it is granted the Web Lock the worker held), hands back the reads it had taken,
and starts a new worker that takes the disc over under the same generation, so a running game goes
on reading it; after three restarts, or when a new worker cannot start, it stops the request block
so that what is waiting fails at once. A disc being opened when its worker died is opened again by
the next one.

## Sound

The design and its measurements are in `docs/web-port/AUDIO-DESIGN.md`. In short:

- PCSX2's SPU2 mixes on the CPU thread (the EE thread) and writes 64-frame chunks to an
  `AudioStream`, time-stretching them on the way. Here that stream is `WebAudioStream` (`Main.cpp`,
  "Audio"), registered for `AudioBackend::Frontend` with `AudioStream::SetFrontendStreamFactory()`.
- The page makes an `AudioContext` at 48 kHz inside the Boot gesture (or, for `?bios=`, without one,
  and it then waits for the first key press or click) and calls `pcsx2web_audio_start()`. That loads
  this module into the context's worklet scope (Emscripten's Wasm Audio Worklet) and creates one
  `AudioWorkletNode` for the page's lifetime, connected to the page's `GainNode` (volume and mute).
- Its callback runs on the browser's audio thread: each 128-frame render quantum it reads the
  attached stream with `ReadFrames()` (lock-free, no allocation), resampling linearly when the
  stream's rate differs (PS1 mode, 44.1 kHz), and smooths the edges into and out of silence.
- Once per vsync the CPU thread attaches the stream while the worklet's heartbeat moves; otherwise
  (a suspended context, autoplay, Stop) it detaches it and drains it at the real-time rate, so the
  stream never builds up a backlog. A seq_cst gate makes sure only one side calls `ReadFrames()`.
- Settings (`InitializeConfig()`): `SPU2/Output` `Backend=Frontend`, `SyncMode=TimeStretch`,
  `StretchUseQuickSeek=true` (a quarter of the stretcher's default cost), `ExpansionMode=Disabled`
  (stereo), `BufferMS=50`.
- Below full speed, the stretcher needs more buffer to stay continuous, so the frontend steps
  `BufferMS` by the measured speed: 50 ms at 75 % and faster, 100 ms from 50 %, 200 ms from 28 %,
  400 ms from 14 %, 800 ms below. It steps up after 1 s below a step's range, and down after 10 s
  well inside a faster step's range (1.5 times its lower bound, at most 15 points more: 90 % for
  50 ms). A step keeps the stream: it is made with the
  largest buffer and only its target changes, so nothing buffered is lost; a step down lowers the
  target gradually (100 ms per second) and the stretcher plays the excess out slightly faster.
- Stop fades the gain out over 30 ms and suspends the context; Boot again resumes it. A hidden tab
  keeps playing.

The audio block (`pcsx2web_audio()`, little-endian u32, read with `Atomics.load`):

| Word | Name | Meaning |
| --- | --- | --- |
| 0, 1 | magic, version | `0x44554154` ("TAUD"), 1 |
| 2 | worklet | 0 not started, 1 starting, 2 running, 3 failed |
| 3 | stream rate | 0 without a stream, else 48000 (44100 in PS1 mode) |
| 4 | attached | 1 while the worklet reads the stream |
| 5 | heartbeat | render quanta seen |
| 6 | frames | frames the worklet read |
| 7 | short quanta | quanta that could not be filled from the stream (underruns) |
| 8 | drained | frames discarded while detached |
| 9 | context rate | the AudioContext's sample rate |
| 10 | buffer ms | the stream's `BufferMS` step |
| 11 | fill | frames buffered in the stream at the last vsync |
| 12, 13, 14 | attaches, steps, streams | counts |

## Current limitations

- No touch controls yet. Rumble only where the browser can vibrate a gamepad (Chromium, not Firefox). Two pads (ports 1 and 2, no multitap). Two memory card slots of 8 MB `.ps2` cards (no multitap, PS1 or folder cards).
- Sound: stereo only (no surround), no output device choice; below about 20 % speed it plays
  continuous but smeared (time-stretched), and when the machine is overloaded it can still break up.
  Safari is untested.
- Discs: single-file images only (no CHD with a parent, blockdump or multi-file BIN/CUE). The disc
  library is for booting: while a game runs, discs change through the disc menu. In the disc library, CHD, ZSO and GZ discs are named after
  their file (no serial, so no cover from ps2-covers); ISO and CSO discs are read for their serial.
- Not every scene runs at full speed on every machine: the EE (main CPU) thread is the usual limit. Guitar
  Hero, GTA San Andreas and Final Fantasy XII's cutscenes run at or near 100 % in Chrome, and a WWE SmackDown
  vs. Raw 2011 match runs smoothly on a Ryzen 7 7840U at full power. A power manager capping the CPU (TLP on
  Linux, even on mains) costs a lot: under such a cap that match read 50-60 % (`docs/web-port/PERFORMANCE.md`).
- The hardware renderer on WebGPU needs dual-source blending (not in Safari) and does not have
  upscaling beyond 2×, PrimID DATE, AA1, CAS or FXAA yet (`docs/web-port/WEBGPU-DESIGN.md`, step 3).
- Only the main BIOS file is loaded (no separate ROM1/ROM2/NVM/MEC files).
- Needs a recent desktop browser with cross-origin isolation (Chrome, Edge or Firefox; Safari is
  untested). WebGPU presentation also needs JSPI (Chrome 137+, Firefox 157 has it); without it,
  or without WebGPU, the page draws on a 2D canvas. The module reserves 512 MB of memory at start,
  which many phones refuse.
- The dev server is for local use only. Elsewhere the page isolates itself with a service worker
  (see [Deploying](#deploying)), which some private windows refuse.
