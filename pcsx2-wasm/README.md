# TabStationToo

TabStationToo is a browser frontend for the WebAssembly build of PCSX2 (the `pcsx2-wasm`
target). It is built on PCSX2 (GPL-3.0) and is not affiliated with Sony or the PCSX2 project.
You pick your own PS2 BIOS and, if you like, a disc image of one of your games, press Boot, and the
game (or the BIOS menu) renders in a `<canvas>`, driven from the keyboard. It runs the interpreters
(no JIT on wasm) with the software renderer, whose output is copied to the page as plain RGBA
pixels. Sound plays through Web Audio. Memory card slot 1 is kept in the browser. There are no save
states yet.

- `Main.cpp`: the frontend (Host callbacks, settings, CPU thread loop, `pcsx2web_*` exports)
- `WebAudioStream.cpp`: the audio output (PCSX2's `WebAudio` backend, see [Audio](#audio))
- `web/`: the page (`index.html`, `app.js`, `disc-worker.js`, `style.css`, `icons.svg`,
  `favicon.svg`, `fonts/`, `icons/`, and `isolation.js` with `coi-serviceworker.js` for hosts
  without isolation headers), copied next to the build output. The design is
  `docs/web-port/UI-SPEC.md` (§10 for discs).
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
response, and never lists directories or serves anything outside the web root. `--no-isolation`
leaves the isolation headers out and `--max-age 600` lets the browser cache files for 10 minutes,
which together behave like GitHub Pages (see [Deploying](#deploying)). For the same
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
to IndexedDB itself: 1.5 s after the game's last write to it, at once when the VM stops (Stop
closes the card, so a normal Stop never loses a save), and when the page is hidden. The page counts
the card's writes (`FS.write`) rather than trusting its time alone: `FS.syncfs` only copies files
whose time changed, in whole milliseconds, so before each copy the page moves the time of every card
written since the last one past any time it had, and a write in the same millisecond as the last copy
is still copied. On wasm PCSX2 rewrites the card's checksum at shutdown only if it changed, so a Stop
without a save copies nothing. A copy that fails is tried again after 2, 5 and 15 s; then the page
stops copying until the game writes to the card again, and says that saves last for the visit. The copy when the page is
hidden or left does not wait for that pause, and its failure does not count toward giving up. A copy of the 8 MB card blocks
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

Everything the page keeps in the browser (the remembered BIOS, the memory card, the last disc's
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
on reading it; after three restarts, or when a new worker cannot start, it stops the request block
so that what is waiting fails at once. A disc being opened when its worker died is opened again by
the next one.

## Audio

SPU2 writes its samples into PCSX2's `AudioStream` ring on the CPU thread, as on desktop. The
`WebAudio` backend (`WebAudioStream.cpp`, linked with `-sAUDIO_WORKLET -sWASM_WORKERS`) reads it
from a Wasm Audio Worklet: the worklet runs on the browser's audio thread and calls
`AudioStream::ReadFrames`, which only touches atomics, so neither side waits for the other and the
page's main thread only makes the asynchronous Web Audio calls. The worklet loads `pcsx2-wasm.js`
like the pthreads do; there is no extra file. There is one `AudioContext` per sample rate (48 kHz,
or 44.1 kHz in PS1 mode), created on the first boot and kept: it is suspended while no VM runs.
Browsers start audio only after a click or key press on the page; the context starts on the next
one if the boot did not come from one.

Settings differ from desktop in one place: a 100 ms buffer instead of 50 ms, because most content
runs below full speed and the time stretcher needs the room. Measured in headless Chrome on the BIOS
intro (about 41 % speed): 177 dropouts in 26 s with 50 ms, 10 with 100 ms and 2 with 200 ms (which
delays sound by a fifth of a second). Time stretching costs under 1 % of speed there.
`pcsx2-headless` keeps the null backend (Node has no Web Audio).

## Current limitations

- No save states, gamepads, touch controls or volume control yet; fixed key layout. One memory card
  (slot 1).
- Sound below full speed is time-stretched (slowed down), and still drops out at times when the
  speed swings.
- Discs: uncompressed single-file images only (no CHD, CSO/ZSO, GZ, blockdump or multi-file
  BIN/CUE), and no disc swap or eject while a game runs.
- Interpreters only, so most content runs below full speed: game menus mostly run at 60–100 %,
  3D battles at 10–20 %.
- Only the main BIOS file is loaded (no separate ROM1/ROM2/NVM/MEC files).
- Needs a recent desktop browser with cross-origin isolation (Chrome, Edge or Firefox; Safari is
  untested). The module reserves 512 MB of memory at start, which many phones refuse.
- The dev server is for local use only. Elsewhere the page isolates itself with a service worker
  (see [Deploying](#deploying)), which some private windows refuse.
