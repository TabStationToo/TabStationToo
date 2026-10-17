# TabStationToo

TabStationToo is a browser frontend for the WebAssembly build of PCSX2 (the `pcsx2-wasm`
target). It is built on PCSX2 (GPL-3.0) and is not affiliated with Sony or the PCSX2 project.
You pick your own PS2 BIOS, press Boot, and the BIOS menu renders in a `<canvas>`, driven from
the keyboard. It runs the interpreters (no JIT on wasm) with the software renderer. Where the
browser offers WebGPU, the GS merges, deinterlaces and presents that renderer's output on the GPU
and posts each frame to the page as an `ImageBitmap` (the WebGPU device,
`docs/web-port/WEBGPU-DESIGN.md`); otherwise the output is copied to the page as plain RGBA
pixels and drawn on a 2D canvas. There is no audio, no disc support and no saving yet.

- `Main.cpp`: the frontend (Host callbacks, settings, CPU thread loop, `pcsx2web_*` exports)
- `web/`: the page (`index.html`, `app.js`, `style.css`, `icons.svg`, `favicon.svg`, `fonts/`,
  `icons/`), copied next to the build output. The design is `docs/web-port/UI-SPEC.md`.
- `../tools/wasm/serve.py`: local dev server with the headers the page needs

## Build

Needs Emscripten 6.0.12 and the wasm dependencies (built once):

```sh
source ~/emsdk/emsdk_env.sh
tools/wasm/build-dependencies-wasm.sh deps-wasm        # once; output in deps-wasm/

emcmake cmake -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/deps-wasm" \
  -DENABLE_QT_UI=OFF -DENABLE_GSRUNNER=OFF -DENABLE_TESTS=OFF
ninja -C build-wasm pcsx2-wasm
```

Use `-DCMAKE_BUILD_TYPE=Devel` (for example in `build-wasm-dbg`) for a build with assertions.
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
response, and never lists directories or serves anything outside the web root. For the same
reason the page loads nothing from other origins: no CDN, and same-origin, self-hosted fonts
(Archivo and Chivo Mono, OFL 1.1, in `web/fonts/`; Phosphor icons, MIT, in `web/icons.svg`).

## Use

1. Open the URL. The emulator (about 14 MB) starts downloading at once; the screen asks for your
   BIOS. Press **Choose BIOS file…** or drop the file on the page (usually a 4 MB `.bin`, for
   example `SCPH-39001.bin`). On touch-only devices the download waits until you pick a BIOS.
2. Press **Boot** (or Enter while the screen has focus). If the emulator is still downloading, the
   boot is queued; press again to cancel. The BIOS menus appear after a few seconds. A file that
   is not a PS2 BIOS (no ROM directory with a ROMVER entry, as PCSX2's own BIOS list requires) is
   refused before the VM starts, and the slate shows the reason from the log.
3. When the picture appears, the keyboard goes to the PS2 while the screen has focus (the amber
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
   hiding the tab also release it. Clicking Stop, Crisp, Keys, Log or Fullscreen keeps the keyboard
   on the PS2. Browser shortcuts (Ctrl/Cmd/Alt combinations, F5, F11, F12) keep working. The controls fade after 2.5 s without
   pointer activity; **Keep controls visible** in the Keys drawer turns that off.
4. Under the screen: **Stop** shuts the VM down (then **Boot again**); the BIOS menu shows where the
   BIOS is stored and has **Choose another file…** and **Forget this BIOS**; **Crisp** switches
   between sharp and smooth scaling; **Keys** (`?`) and **Log** (`` ` ``, ² on AZERTY) open the
   drawers; **Fullscreen** (Alt+Enter, or a double-click on the picture) fills the display. The
   single-key shortcuts only work while focus is on the screen or the controls under it.

Use only a BIOS dumped from a console you own. The file never leaves the browser: it is written
into the emulator's in-memory file system (`/bios/<name>`) and remembered in the site's IndexedDB
so you do not have to pick it again. **Forget this BIOS** deletes it.

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
| `?set=Section/Key=Value` | Overrides one setting after the defaults (repeatable), e.g. `?set=EmuCore/GS/deinterlace_mode=2` |
| `?surface=WxH` | Presents at a fixed size on both paths (e.g. `640x448` with `?set=EmuCore/GS/AspectRatio=Stretch` for 1:1) |
| `?capture=N,M,...` | Keeps a copy of the first frame presented at vsync N or later, read with `window.PCSX2Web.capture(N)` |
| `?gsidle=block` | Keeps the GS thread's idle wait blocking with WebGPU, leaving only the device's bounded event-loop turn |

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
the adapter's largest texture; the page scales a smaller frame to the canvas). **Crisp** switches
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

## UI assets

The page self-hosts everything it loads (it is cross-origin isolated and makes no requests to
other origins). Fonts and icons come from pinned packages managed with pnpm in `assets/`:

| Asset | Package | Licence | In `web/` |
|---|---|---|---|
| Archivo (variable, Latin) | `@fontsource-variable/archivo` | SIL OFL 1.1 | `fonts/archivo.woff2`, `fonts/Archivo-OFL.txt` |
| Chivo Mono (variable, Latin) | `@fontsource-variable/chivo-mono` | SIL OFL 1.1 | `fonts/chivo-mono.woff2`, `fonts/ChivoMono-OFL.txt` |
| Phosphor icons (Regular) | `@phosphor-icons/core` | MIT | `icons.svg`, `icons/Phosphor-LICENSE.txt` |

`icons.svg` is generated: the Phosphor icons listed in `assets/sync-assets.mjs` followed by the
in-house symbols in `assets/icons-local.svg` (face-button glyphs and the mark). To update an
asset, change its version in `assets/package.json`, then in `assets/`:

```sh
pnpm install
pnpm run sync
```

Commit the regenerated files in `web/` together with `package.json` and `pnpm-lock.yaml`.

## How the page talks to the emulator

`main()` runs on a worker (`-sPROXY_TO_PTHREAD`), so the browser main thread only runs `app.js`.
It calls exports that never block (atomics and a lock-free key queue only):

| Export | Use |
| --- | --- |
| `pcsx2web_boot(name)` | boot `/bios/<name>`; 0 if accepted, -1 otherwise |
| `pcsx2web_stop()` | request shutdown |
| `pcsx2web_state()` | 0 waiting, 1 starting, 2 running, 3 stopped, 4 failed (polled every 500 ms) |
| `pcsx2web_speed()`, `pcsx2web_vps()` | speed in percent, vertical syncs per second |
| `pcsx2web_key_code(name)`, `pcsx2web_key(code, pressed)` | resolve host key names once, then queue key events |
| `pcsx2web_frame()` | address of the frame descriptor: `seq, width, height, pixels, vsync` (u32) |
| `pcsx2web_surface_size(w, h, scale)` | the display canvas's backing size and device pixel ratio (WebGPU presents at it) |
| `pcsx2web_set_crisp(on)` | nearest (1) or bilinear (0) presentation, applied at the next vsync |
| `pcsx2web_vsync_count()` | vsyncs emulated since the module started |

On the 2D path, each animation frame the page reads `seq` with `Atomics.load`; when it changed, it copies
`width * height * 4` bytes of RGBA from `pixels` into an `ImageData` (which cannot use shared
memory), re-reads `seq` and drops the copy if a newer frame may have overwritten it, then draws it.
Views on the wasm memory are recreated every frame because memory growth replaces the buffer.

## Current limitations

- No audio, discs, memory cards, save states, gamepads or touch controls yet; fixed key layout.
- Interpreters only, so most content runs below full speed; the BIOS menu is the target for now.
- The GS always uses the software renderer; WebGPU only merges, deinterlaces and presents its output
  (the hardware renderer on WebGPU is the next step of `docs/web-port/WEBGPU-DESIGN.md`).
- Only the main BIOS file is loaded (no separate ROM1/ROM2/NVM/MEC files).
- Needs a recent desktop browser with cross-origin isolation (Chrome, Edge or Firefox; Safari is
  untested). WebGPU presentation also needs JSPI (Chrome 137+, Firefox 157 has it); without it,
  or without WebGPU, the page draws on a 2D canvas. The module reserves 512 MB of memory at start,
  which many phones refuse.
- The dev server is for local use only; hosting elsewhere needs the same COOP/COEP headers.
