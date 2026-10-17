# TabStationToo

TabStationToo is a browser frontend for the WebAssembly build of PCSX2 (the `pcsx2-wasm`
target). It is built on PCSX2 (GPL-3.0) and is not affiliated with Sony or the PCSX2 project.
You pick your own PS2 BIOS and, if you like, a disc image of one of your games, press Boot, and the
game (or the BIOS menu) renders in a `<canvas>`, driven from the keyboard. It runs the interpreters
(no JIT on wasm) with the software renderer, whose output is copied to the page as plain RGBA
pixels. Memory card slot 1 is kept in the browser. There is no audio and there are no save states
yet.

- `Main.cpp`: the frontend (Host callbacks, settings, CPU thread loop, `pcsx2web_*` exports)
- `web/`: the page (`index.html`, `app.js`, `disc-worker.js`, `style.css`, `icons.svg`,
  `favicon.svg`, `fonts/`, `icons/`), copied next to the build output. The design is
  `docs/web-port/UI-SPEC.md` (§10 for discs).
- `../pcsx2/CDVD/Emscripten/WebDisc.*`, `WebDiscFileReader.*`: the emulator's side of the disc
  reader; `web/disc-worker.js` is the page's side (`docs/web-port/DISC-DESIGN.md`).
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
2. To play a game, press **Choose a disc…** on the BIOS's title card, or drop the image on the page:
   an uncompressed `.iso`, `.bin` or `.img` made from a disc you own. The page checks that the file
   holds an ISO 9660 volume where PCSX2 looks for one and refuses anything else (compressed
   `.chd`/`.cso`/`.zso`/`.gz` and `.dump` images get their own message). The file is not copied or
   uploaded: the disc worker reads the parts the game asks for, while it runs. In Chrome and Edge
   the picker (or a drop) also gives the page a handle to the file, kept in IndexedDB: after a
   reload the title card offers **Open it again**, which asks the browser for permission and opens
   the disc with one press. Other browsers need the file picked again. The disc menu under the
   screen (the disc's title) has **Choose another disc…** and **Remove the disc**; neither works
   while the PS2 runs, because there is no disc swap yet.
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
   hiding the tab also release it. Clicking Stop, Crisp, Keys, Log or Fullscreen keeps the keyboard
   on the PS2. Browser shortcuts (Ctrl/Cmd/Alt combinations, F5, F11, F12) keep working. The controls fade after 2.5 s without
   pointer activity; **Keep controls visible** in the Keys drawer turns that off.
5. Under the screen: **Stop** shuts the VM down (then **Boot again**); the BIOS menu shows where the
   BIOS is stored and has **Choose another file…** and **Forget this BIOS**; **Crisp** switches
   between sharp and smooth scaling; **Keys** (`?`) and **Log** (`` ` ``, ² on AZERTY) open the
   drawers; **Fullscreen** (Alt+Enter, or a double-click on the picture) fills the display. The
   single-key shortcuts only work while focus is on the screen or the controls under it.

Use only a BIOS dumped from a console you own. The file never leaves the browser: it is written
into the emulator's in-memory file system (`/bios/<name>`) and remembered in the site's IndexedDB
so you do not have to pick it again. **Forget this BIOS** deletes it.

### Memory card

Slot 1 holds an 8 MB card, `/memcards/Mcd001.ps2`; slot 2 is empty. `/memcards` is an IDBFS mount
(Emscripten's IndexedDB-backed file system, database `/memcards`), loaded before `main()` runs.
PCSX2 keeps the card file open and writes it in place (unbuffered on wasm), so the page copies it
to IndexedDB itself: 1.5 s after the card's modification time stops changing, at once when the VM
stops (Stop closes the card, so a normal Stop never loses a save), and when the page is hidden.
`FS.syncfs` only copies files whose time changed, and on wasm PCSX2 rewrites the card's checksum at
shutdown only if it changed, so a Stop without a save copies nothing. A copy of the 8 MB card blocks
the page's thread for 30–50 ms (measured: 28–31 ms on an idle machine, 42–51 ms with another emulator
running), once per save, so the picture may skip a frame or two then. The copy is asynchronous and
cannot finish while the page unloads, so from the first card write (seen as it happens, through
`FS.write`) until the copy is done the page asks before it is closed or reloaded (`beforeunload`).

Only one tab keeps the card: the page holds the Web Lock `tabstationtoo-memcards` for as long as it
is open (waiting up to 2 s for it at start-up, for the page a reload replaces). A second tab loads
the saved card but never copies its own back, so it cannot overwrite the first tab's saves (desktop
PCSX2 gets the same from opening the card with `DenyWrite`), and its title card says that its saves
last for the visit.

Why IDBFS rather than OPFS: the card is written through PCSX2's `FILE*` calls, which this build's
file system (the legacy JS one, MEMFS) runs on the page's thread. OPFS can only be written
synchronously from a worker, so it would need WasmFS (a separate decision, PLAN §5) or a second
copy path; IDBFS persists the same MEMFS files asynchronously, works in every browser with
IndexedDB, and the card is small. If IndexedDB is unavailable, the card lasts for the visit and the
disc's title card says so.

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
VPS, frames presented and dropped, frame size) and read `window.PCSX2Web.logText()`.

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
the game's title and serial as PCSX2 reported them, and the memory card's last save.

VU1 JIT testing (`docs/web-port/VU1-JIT-DESIGN.md`; the JIT is off by default and this is not a user
setting):

- `?vu1jit=1` boots with the VU1 JIT (`EmuCore/CPU/Recompiler/EnableVU1`). Its counters (self-test,
  programs compiled, instructions, of them on the interpreter) go to the browser console every 10 s.
- `?vu1jit=fuzz:SEED:COUNT[:timing|jit|both]` runs the JIT's fuzzer on a worker, without a BIOS or
  a VM (boots are refused while it runs), and logs its summary: `timing` checks the timing model at
  every instruction of COUNT random programs, `jit` runs each one on the interpreter and on the JIT
  and compares them, `both` does both.
- `window.PCSX2Web.vu1jit()` returns the mode, the counters and the fuzzer's status.

The headless runner has the same switches and more: `-set EmuCore/CPU/Recompiler/EnableVU1=true`,
`-vu1jit-fuzz SEED:COUNT[:MODE]`, `-vu1jit-oracle` (the timing model checked at every VU1
instruction of a real run), `-vu1jit-lockstep` (every VU1 program run on the JIT and on the
interpreter and compared), `-vu1jit-stats FILE` (per-vsync VU1 statistics as CSV) and `-mtvu-sync`
(the EE waits for the VU thread at each check, so whole runs repeat exactly).

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
| `pcsx2web_boot(name)` | boot `/bios/<name>` with no disc; 0 if accepted, -1 otherwise |
| `pcsx2web_boot_disc(name, path, flags)` | boot the disc at `path` (`/disc/<file>`, see below) with `/bios/<name>`; `flags` 1 = full boot (BIOS intro), 0 = fast boot; 0 if accepted, -1 otherwise |
| `pcsx2web_disc_block()` | address of the disc request block the disc worker serves (`DISC-DESIGN.md` §4.1) |
| `pcsx2web_game()` | address of the running game's details: `seq, disc CRC, ELF CRC` (u32), `serial[32]`, `title[256]` (UTF-8); `seq` is odd while they change |
| `pcsx2web_stop()` | request shutdown |
| `pcsx2web_state()` | 0 waiting, 1 starting, 2 running, 3 stopped, 4 failed (polled every 500 ms) |
| `pcsx2web_speed()`, `pcsx2web_vps()` | speed in percent, vertical syncs per second |
| `pcsx2web_key_code(name)`, `pcsx2web_key(code, pressed)` | resolve host key names once, then queue key events |
| `pcsx2web_frame()` | address of the frame descriptor: `seq, width, height, pixels, 0` (u32) |

Each animation frame the page reads `seq` with `Atomics.load`; when it changed, it copies
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
on reading it; after three restarts it fails what is waiting instead.

## Current limitations

- No audio, save states, gamepads or touch controls yet; fixed key layout. One memory card (slot 1).
- Discs: uncompressed single-file images only (no CHD, CSO/ZSO, GZ, blockdump or multi-file
  BIN/CUE), and no disc swap or eject while a game runs.
- Interpreters only, so most content runs below full speed: game menus mostly run at 60–100 %,
  3D battles at 10–20 %.
- Only the main BIOS file is loaded (no separate ROM1/ROM2/NVM/MEC files).
- Needs a recent desktop browser with cross-origin isolation (Chrome, Edge or Firefox; Safari is
  untested). The module reserves 512 MB of memory at start, which many phones refuse.
- The dev server is for local use only; hosting elsewhere needs the same COOP/COEP headers.
