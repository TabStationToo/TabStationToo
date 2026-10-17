# Disc access in the browser

*Design note for TabStationToo, written 2026-10-16. Phase 1 (this note, the JS side, the dev
server and the test harness) is done. Phase 2, the C++ side and the page (section 5), is built for
flat images and tested with a real game in the browser; section 5.6 lists what differs from the
plan and what is still open.*

**Summary.** PCSX2 reads discs on demand through a `ThreadedFileReader` running on a pthread. In
the browser that pthread will post byte-range requests into a small request block in the shared
wasm memory and sleep in `Atomics.wait`. A dedicated disc worker
(`pcsx2-wasm/web/disc-worker.js`) holds the disc, wakes on the block's doorbell, reads the range
and copies it straight into the wasm memory. The disc can be a `File` the user picked, a
`FileSystemFileHandle`, an OPFS file or a URL served with byte ranges. Nothing is copied
beforehand. The protocol itself costs 15 µs per request. A read from a user's file costs
0.2–0.5 ms for 2 KiB and 0.4–0.9 ms for 128 KiB, and sequential reads run at 136–274 MB/s. A
PS2 DVD drive delivers about 5.5 MB/s. WORKERFS cannot work with the proxied file system, and
this note shows it by experiment. Copying discs into OPFS gives much faster reads, but the copy
costs time and quota. Chrome 152 also caps one OPFS file at 2000 MiB. WasmFS has no backend for
user files.

Contents: 1 problem · 2 options and evidence · 3 measurements · 4 design · 5 Phase 2 C++/JS
interface · 6 page UX · 7 risks · 8 files and how to test.

## 1. The problem

- PCSX2 opens a disc image with `InputIsoFile::Open`. `GetFileReader` (`pcsx2/CDVD/InputIsoFile.cpp:40`)
  picks a `ThreadedFileReader` subclass by extension: CHD, CSO/ZSO, GZ, blockdump, or flat
  (`FlatFileReader`). `ThreadedFileReader` runs its own reader thread ("ISO Decompress") with two
  readahead buffers of at least 128 KiB. Its subclasses implement `ChunkForOffset`/`ReadChunk`.
  `FlatFileReader` reads 128 KiB chunks with `fseek`/`fread`. CDVD reads sectors of 2048–2448
  bytes, but the subclass sees whole chunks. The CPU thread blocks in `FinishRead`/`ReadSync`
  when the data is not in the readahead buffers, so any read latency adds to emulation time.
- A DVD image is 1–8.5 GB (the test disc is 3,199,631,360 bytes). The wasm heap is wasm32 and
  grows to at most 2 GB, so the image cannot be loaded into memory.
- The page is linked with `-pthread -sPROXY_TO_PTHREAD` and the legacy JS file system (MEMFS).
  With pthreads, Emscripten proxies every file syscall from a pthread to the page's main thread
  and waits for the answer (`wrapSyscallFunction` in `src/lib/libcore.js`, emsdk 6.0.12). The page
  thread must never block, and it cannot read a `File` synchronously: `FileReaderSync` exists
  only in workers.
- Requirements: read sectors on demand and synchronously from the reader pthread, from a file the
  user picks, and from a URL for automated tests.

## 2. Options and evidence

### (a) Dedicated disc worker plus a shared request block: chosen

The page creates one worker and sends it the emulator's `WebAssembly.Memory` (shared, so it can
be posted), the address of a request block that the C++ side owns, and the disc. Requesters (C++
pthreads) fill a slot, ring a doorbell word (`Atomics.add` + `notify`) and wait on the slot's
state word. The worker waits on the doorbell with `Atomics.waitAsync`, or blocks in
`Atomics.wait` when `waitAsync` is missing. It reads the range with `FileReaderSync` or
`blob.slice().arrayBuffer()` for files, with `fetch` plus `Range` (or a synchronous XHR) for URLs,
or with `FileSystemSyncAccessHandle.read` for OPFS files. It then copies the data into the wasm
memory and notifies the slot.

Evidence (section 3): it works in Chrome 152 (the Claude desktop browser pane, Electron), Chrome
155 (headless) and Firefox 157 (headless). It passed 70 checks in each of 4 configurations in the
pane, and 46 checks in each of 4 configurations in each headless browser. The checks cover the
ISO 9660 primary volume descriptor, hashes of ranges at the start, middle and end against
`serve.py`/curl, short reads at the end, memory growth during reads, 4 concurrent requesters,
cancellation, stop, changed or deleted files, and network loss with recovery. It needs no copy
and no quota, starts at once, and does not depend on the file-system choice (legacy FS or
WasmFS).

### (b) WORKERFS: does not work here

- From the source (emsdk 6.0.12): `WORKERFS.mount` starts with `assert(ENVIRONMENT_IS_WORKER)` and
  creates a `FileReaderSync` (`src/lib/libworkerfs.js`). Syscalls from pthreads are proxied to
  the page thread's FS (`wrapSyscallFunction`). Under WasmFS, `-lworkerfs` is a build error
  ("using -lworkerfs is not currently supported in WasmFS").
- By experiment: a 30-line C program built with `emcc -pthread -sPROXY_TO_PTHREAD -lworkerfs.js`
  and run in the pane (Chrome 152):
  - A, mounted by the page in `preRun`: `FS.mount(WORKERFS, …)` aborts the runtime with
    `Aborted(Assertion failed)`. With `-sASSERTIONS=0` it still aborts, because the library's
    `assert` is not compiled out. `typeof FileReaderSync` on the page is `'undefined'` in all three
    browsers (harness `environment` row).
  - B, mounted from `main()`'s pthread with `EM_ASM`: the mount succeeds and that worker's JS FS
    sees the file, but `fopen("/pt/disc.bin")` from C **fails**. The syscall runs against the page
    thread's FS, which has no such file.
- So the files would have to live in the page's FS, where nothing can read a `File`
  synchronously. Even if that worked, every `fread` would be a round trip through the page
  thread, which is busy drawing frames.

### (c) Copy into OPFS, read with a sync access handle: fastest reads, not the main path

- Reads are the fastest of all options: in Chrome 155, 20 µs for 2 KiB and 35 µs for 128 KiB, at
  3.2 GB/s sequential. In Firefox 157, 60/80 µs. The worker supports it as a source (`handle:`
  an OPFS `FileSystemFileHandle`) and reads straight into the wasm memory.
- Copy cost for 3.2 GB, measured from the dev server URL:
  - Chrome 155: 7.7 s (398 MiB/s).
  - Firefox 157: 13.8 s (221 MiB/s).
  - Chrome 152 pane: 82 MiB/s, and it cannot store the disc at all (next point).
  - For scale: plain sync access handle writes run at 262–704 MiB/s, and plain URL streaming at
    544–955 MiB/s.
- Quota: `navigator.storage.estimate()` reported 10 GB in Firefox ("best effort", at most 10 GiB),
  10–13 GB in headless Chrome and 5.6–8.1 GB in the pane. The copy doubles the disc's footprint,
  and a DVD9 plus saves can exceed the quota. Safari evicts OPFS data after 7 days without use
  (PLAN §3.6).
- In the pane (Chrome 152) **one OPFS file cannot grow past 2,097,152,000 bytes (2000 MiB)**.
  This holds even with OPFS empty, and a second file could still grow.
  - `FileSystemSyncAccessHandle.write` past that point returns 4294967288, which is (u32)-8 or
    `FILE_ERROR_NO_SPACE`, instead of throwing. The file stays at 2000 MiB and `truncate` to a
    larger size silently does nothing.
  - `createWritable()` throws `QuotaExceededError` at 2100 MiB.
  - After the "complete" 3.2 GB import, `estimate()` reported 2.0 GB of usage.
  - An import that does not check every `write()` return value looks successful and leaves a
    truncated disc. The harness's import checks the return values and the final size.
  - Headless Chrome 155 and Firefox 157 stored the full 3.2 GB.
- Verdict: OPFS is not the main path. A later "keep this disc on this device" option could use it
  where it works, through the same worker.

### (d) WasmFS backends: none fits

- **OPFS backend**: reads only files already in OPFS, so it is option (c).
- **Fetch backend** (`libwasmfs_fetch.js`, `fetch_backend.cpp`): reads URLs only, in 16 MiB chunks
  by default. Every chunk it fetches stays in a JS array until the file is freed (`chunks[i] =
  bytes.slice(…)`, released only in `freeFile`), so a long session caches gigabytes outside the
  wasm heap.
- **JS-file and memory backends**: hold the whole file in memory.
- **JSImpl backend**: runs JS synchronously on the calling thread, so the `File` would have to
  live inside the emulator's pthread worker. Emscripten offers no supported way to hand an object
  to a specific pthread's worker.
- WORKERFS is unsupported under WasmFS (see (b)).
- Moving the whole frontend to WasmFS is a separate decision (PLAN §5, open question 11). The disc
  worker works the same under either file system.

### Recommendation

Use (a): the disc worker with a shared request block, reading the user's `File` in place. URL
sources are for automated tests and for users who serve their own discs over HTTP. OPFS is a
possible later option for keeping a copy on the device.

## 3. Measurements

**Setup.**
- Machine: Ryzen 7 PRO 7840U, NVMe, 30 GB RAM. Other agents were running emulator benchmarks at
  the same time (load average 5–7), and every process of mine ran at `nice -n 19`.
- The ISO was in the OS page cache, so these are warm-cache numbers. A cold NVMe adds about
  0.1 ms per random read; a hard disk or USB drive adds much more.
- URLs are served by `tools/wasm/serve.py` on 127.0.0.1, a single-process Python server, not a
  real remote server.
- Times are taken in the requester thread, the C++ thread's point of view: from posting a request
  to waking up with the data in memory. JS and C (`requester.c`, real `memory.atomic.wait32`)
  requesters measured the same.
- Random reads use 2048-aligned offsets across the whole disc. "Seq" is 256 sequential 128 KiB
  reads.
- Sources:
  - **File**: a disk-backed `File` read with `FileReaderSync`. This is the user's case; the pane
    and the harness cannot open a file picker, so the File comes from OPFS `getFile()`.
  - **Blob**: the whole disc fetched into a `Blob`, which Chrome and Firefox keep on disk at this
    size.
  - **SAH**: an OPFS sync access handle.
  - **URL**: `fetch` with `Range`.
- Numbers are medians, with p95 in parentheses, in ms. Run-to-run variation is about ±10 %.

**Protocol round trip** (zero-length request, no source read): 0.015 ms (p95 0.02–0.025) in both
Chromes, with JS or C requesters and either worker loop. Firefox 157: 0.02 ms (its timers are
coarsened to 20 µs).

**Chrome 155 (headless)**, the full 3.2 GB disc in every source:

| Source | 2 KiB | 64 KiB | 128 KiB | 1 MiB (MB/s) | Seq 128 KiB |
|---|---|---|---|---|---|
| File, FileReaderSync | 0.45 (0.58) | 0.63 (0.87) | 0.88 (1.35) | 2.42 (328) | 167 MB/s |
| File, slice().arrayBuffer() | 0.48 (0.69) | 0.64 (0.85) | 0.73 (1.06) | 2.33 (412) | 168 MB/s |
| Blob, FileReaderSync | 0.37 (0.59) | 0.57 (0.95) | 0.69 (1.20) | 2.38 (393) | 136 MB/s |
| SAH (OPFS copy) | 0.020 (0.025) | 0.025 (0.055) | 0.035 (0.065) | 0.13 (6663) | 3179 MB/s |
| URL, fetch | 0.90 (1.24) | 1.03 (1.65) | 1.16 (1.74) | 3.91 (244) | 89 MB/s |
| URL, sync XHR | 1.13 (1.67) | 1.39 (1.97) | 1.58 (2.21) | 6.50 (148) | 72 MB/s |

**Firefox 157 (headless)**, full disc:

| Source | 2 KiB | 64 KiB | 128 KiB | 1 MiB (MB/s) | Seq 128 KiB |
|---|---|---|---|---|---|
| File, FileReaderSync | 0.22 (0.50) | 0.38 (1.16) | 0.56 (1.50) | 2.86 (344) | 166 MB/s |
| File, slice().arrayBuffer() | 0.24 (0.52) | 0.32 (0.78) | 0.48 (0.94) | 2.90 (278) | 274 MB/s |
| Blob, FileReaderSync | 0.18 (0.30) | 0.28 (0.48) | 0.40 (0.62) | 2.16 (442) | 214 MB/s |
| SAH (OPFS copy) | 0.06 (0.12) | 0.06 (0.12) | 0.08 (0.12) | 0.20 (4286) | 1336 MB/s |
| URL, fetch | 1.92 (3.12) | 2.28 (5.00) | 2.32 (4.22) | 5.92 (166) | 47 MB/s |
| URL, sync XHR | 2.20 (4.06) | 2.48 (4.62) | 2.84 (4.48) | 6.44 (150) | 41 MB/s |

**Chrome 152 (the Claude desktop browser pane, Electron).** The File and SAH rows use a 1900 MiB
OPFS copy, because of the 2000 MiB cap. Fetching the whole disc into a `Blob` failed with
`net::ERR_FAILED`.

| Source | 2 KiB | 64 KiB | 128 KiB | 1 MiB (MB/s) | Seq 128 KiB |
|---|---|---|---|---|---|
| File, FileReaderSync | 0.45 (0.63) | 0.61 (0.91) | 0.74 (1.18) | 2.66 (340) | 156 MB/s |
| File, slice().arrayBuffer() | 0.46 (0.62) | 0.59 (0.85) | 0.76 (1.15) | 2.97 (305) | 140 MB/s |
| SAH (OPFS copy) | 0.30 (0.42) | 0.36 (0.62) | 0.76 (0.97) | 2.31 (415) | 194 MB/s |
| URL, fetch | 3.24 (4.21) | 3.27 (4.63) | 3.55 (4.98) | 5.96 (165) | 36 MB/s |
| URL, sync XHR | 3.50 (4.80) | 3.60 (4.97) | 4.23 (98.3) | 8.77 (65) | 7 MB/s (p95 95 ms) |

**Other results.**
- **Parallel URL reads**, 4 requesters with 1 MiB each: 229 MB/s (pane), 295 MB/s (Chrome 155),
  329 MB/s (Firefox).
- **Concurrent mixed reads**: 4 requesters, each reading the same 24 × 256 KiB ranges in a
  different order, all hashes equal, 4 requests in flight at once.
- **Nagle**: before `serve.py` set `TCP_NODELAY`, every 2 KiB URL read took 43 ms. Headers and
  body went out in separate writes, and the body waited for the browser's delayed ACK.
- **Copying into memory first**: fetching the whole 3.2 GB into a `Blob` took 6.1 s in Chrome 155
  and 10.6 s in Firefox, and failed in the pane.

What these numbers mean:
- The File path is 25–50 times faster than a PS2 drive streams: 136–274 MB/s for 128 KiB chunks
  against about 5.5 MB/s at 4x DVD speed.
- A miss in `ThreadedFileReader`'s readahead costs the CPU thread 0.5–0.9 ms per 128 KiB chunk.
  PCSX2 itself emulates seek times of tens of milliseconds.
- FileReaderSync and `slice().arrayBuffer()` cost the same, so the worker defaults to
  FileReaderSync. It is simpler and also works in the blocking loop.
- For URLs, `fetch` beats synchronous XHR in every browser, and in the pane XHR has p95 tails of
  65–98 ms. The worker defaults to `fetch`.
- 2 KiB reads cost about as much as 64 KiB reads, so small reads should be merged (see 5.5).

## 4. Design

```
 page thread (app.js)                        disc worker (disc-worker.js)
   new Worker, postMessage ───────────────▶  holds File / handle / URL
   {attach, memory, block}, {open, file}      waits on the doorbell (Atomics.waitAsync)
   ◀── {status}, {reply}                       reads the range, copies it into wasm memory
                                               ▲ notify(slot.state)  │ notify(doorbell) ▼
 reader pthread(s) (ThreadedFileReader)  ◀──▶  request block in the shared wasm memory
   claim slot, fill it, ring the doorbell, emscripten_futex_wait(slot.state)
```

### 4.1 Request block, version 1

The C++ side owns the block: zero-initialised static storage, 64-byte aligned. Its first four
words are set at compile time. All words are 32-bit little-endian and accessed atomically on both
sides.

Header (64 bytes, 16 words):

| Word | Name | Written by | Meaning |
|---|---|---|---|
| 0 | `MAGIC` | C++ | `0x4b534454` ("TDSK") |
| 1 | `VERSION` | C++ | 1 |
| 2 | `SLOTS` | C++ | number of slots, 1–16 (Phase 2: 4) |
| 3 | `SLOT_BYTES` | C++ | ≥ 32, a multiple of 4 (32 in v1) |
| 4 | `STATE` | worker | 0 detached, 1 no disc, 2 ready, 3 stopped |
| 5 | `DOORBELL` | requesters | `Atomics.add(1)` then notify; the worker waits on it |
| 6 | `GENERATION` | worker | bumped on every open, close and stop (never 0 once attached) |
| 7, 8 | `SIZE_LO`, `SIZE_HI` | worker | disc size in bytes; valid while `STATE` = ready |
| 9 | `MAX_REQUEST` | worker | largest length accepted (16 MiB) |
| 10 | `ERROR` | worker | last I/O error code since the disc opened, 0 if none |
| 11–15 | reserved | | 0 |

Slot *i*, at byte 64 + *i* × `SLOT_BYTES` (8 words):

| Word | Name | Meaning |
|---|---|---|
| 0 | `STATE` | 0 free, 1 claimed, 2 pending, 3 busy, 4 done; the requester waits on this word |
| 1 | `RESULT` | bytes read (fewer only at the end of the disc) or a negative error code |
| 2 | `DST` | wasm address of the destination (never 0) |
| 3 | `LENGTH` | bytes requested (≤ `MAX_REQUEST`) |
| 4, 5 | `OFFSET_LO`, `OFFSET_HI` | byte offset in the disc |
| 6 | `GENERATION` | the `GENERATION` the requester opened the disc with |
| 7 | `CANCEL` | requester sets 1 (cancel) or 2 (its deadline ran out), then rings the doorbell, to cancel a busy request; the worker answers `CANCELLED` or `TIMEOUT` |

Error codes: −1 `IO` (file unreadable or changed, HTTP error), −2 `INVALID` (destination null or
outside the memory, length too large), −3 `NO_DISC`, −4 `STALE` (the disc changed since the
requester opened it), −5 `CANCELLED` (requester cancel or worker stop), −6 `NETWORK`, −7
`TIMEOUT`. `requester.c` also uses −8 `NO_SLOT`; the C++ reader instead waits for a free slot.

State machine of a slot:
- The requester claims the slot with `CAS FREE→CLAIMED`, writes the fields, then stores
  `PENDING`.
- The worker takes it with `CAS PENDING→BUSY`. A requester can withdraw its request with
  `CAS PENDING→FREE`, and exactly one of the two CAS operations wins.
- A `BUSY` slot always ends in `DONE`: the worker stores `RESULT`, then `STATE`, then notifies.
- The requester reads `RESULT` and stores `FREE`.

Data copied before the `DONE` store is visible after the requester's atomic load sees `DONE`
(sequentially consistent atomics on both sides).

### 4.2 The worker

- **Memory growth.** Before every use, the worker compares `memory.buffer` with the buffer its
  views were made from, and makes new views when it changed. Growth replaces the
  `SharedArrayBuffer` object (`memory.buffer === memory.buffer` holds until then; harness
  `environment` row). Tested by growing the memory by 64 MiB during 400 reads, then reading into
  an address above the old end. The worker's `waitAsync` on a view of the old buffer is still
  woken by notifies through the new one.
- **Loop.** With `Atomics.waitAsync` (Chrome and Firefox 157 measured; Safari 16.4+ according to
  its documentation, untested), the worker never blocks
  its event loop, so page messages are handled at once. Without it, the worker blocks in
  `Atomics.wait` for at most 50 ms at a time and yields to its event loop between waits, and a
  5 ms timer watches for cancellation while it awaits a read. Both loops pass the same tests.
- **Concurrency.** Up to 16 slots. Async reads from different slots overlap (4 in flight
  measured). `FileReaderSync` reads are sequential, which costs nothing because they are
  sub-millisecond.
- **Validation.** Before touching memory, the worker rejects a null `dst`, `dst + length` past the
  memory, `length > MAX_REQUEST` and unsafe offsets. It clamps reads to the disc size; at or past
  the end the result is 0.
- **A disc that changes under the reader.**
  - Chrome raises `NotReadableError` or `NotFoundError` for a file that was changed or deleted
    after it was picked. Firefox instead returns short reads when the file shrank (measured).
  - The worker treats any short read inside the known size as `IO`, sets state `error` and gives
    a "pick it again" message.
  - An OPFS file rewritten in place at the same size is not detected in Chrome 152: reads return
    the new bytes (measured).
- **URL sources.**
  - Opening asks for `bytes=0-0`: a 206 response gives the size; a 200 means the server ignores
    ranges and is refused.
  - Each read checks `Content-Range`. A 416 inside the known size counts as a changed disc.
  - Network errors and 408/429/5xx responses are retried after 250 ms and 1 s.
  - Each attempt has a 30 s timeout (8 s for a synchronous XHR, which blocks the worker, and a
    timeout is not retried there). Opening has a 15 s timeout. Cancellation aborts the `fetch`.
- **Unavailable sources.** A read that ends in `TIMEOUT` (its own timeout, or `CANCEL` = 2 from a
  requester whose deadline ran out) or `NETWORK` marks the source unavailable and sets state
  `error`. Until a probe (the first byte, 1 s later and then every 2 s, 5 s each) finds it
  answering with the same size, new requests fail at once with that code. PCSX2 retries a failed
  read up to 256 times; without this each retry would wait out the deadline again.
- **`check`.** Reads the first sector (a `fetch` of byte 0 for URLs) within 5 s and replies ok or
  with the error, setting state `error` or clearing it. The page asks before every disc boot.
- **Life lock.** With `attach.lock`, the worker takes that Web Lock before it writes anything into
  the memory and holds it until it ends; `status.lock` says whether it got it. The browser releases
  the lock only once nothing of the worker runs any more, so a page granted the same lock after
  `terminate()` knows the worker can no longer write into the memory, even from a synchronous read.
- **Resuming.** With `attach.resume` (`{file | handle | url, name, generation, size}`), a worker that
  replaces a dead one opens that source and, if the block still says ready with that generation and
  size and the source has that size, serves it under the same generation: requests waiting in the
  block, and the open disc in the emulator, stay valid. Otherwise it attaches with no disc (the
  reply is ok, `status.disc` is null and `status.error` says why).
- **A loop that dies** (an exception escaping the serving loop) hands back the requests the worker
  had taken (`BUSY`→`PENDING`, their late results dropped) and reports `stopped`, so the page
  starts a worker that resumes the disc.
- **Stop.** The worker sets `STATE = stopped`, fails pending and busy requests with `CANCELLED`
  (measured: the waiting requester woke 0.4–1.1 ms after `stop`), aborts fetches and ends its
  loop. Later requests fail at once because requesters check `STATE` first. The page must send
  `stop` before `terminate()` (see 7).
- **Status for the page.** Messages are `{type: 'status', status}` on every change of state or
  error, plus `{type: 'reply', id, ok, error, status}` for each message that carried an `id`.
  - `status = {state, disc, error, stats, loop, features}`.
  - `state` is `detached`, `idle`, `opening`, `ready`, `error` or `stopped`. `error` means a disc
    is open but its last read failed; the next successful read returns it to `ready`.
  - `disc = {kind, name, size, generation, read}`.
  - `error = {code, message}`, with a message meant for the user.
  - `stats = {reads, bytes, errors, inflight, maxInflight, busyMs}`.
  - `features = {waitAsync, fileReaderSync, syncXhr}`.

### 4.3 Page messages

| Message | Effect |
|---|---|
| `{type: 'attach', memory, block, loop?, lock?, resume?}` | validates the block (magic, version, layout, bounds), takes the life lock (`lock`), sets `MAX_REQUEST`, `STATE` = no disc (or, with `resume`, keeps serving that disc under its generation), starts the loop (`loop: 'blocking'` forces the fallback) |
| `{type: 'open', file \| handle \| url, name?, read?}` | opens the new disc while the old one keeps being served; once it is open, drops the old one (its requests fail `STALE`) and publishes size and generation in one step, `STATE` = ready. An open that fails leaves the old disc in place; one overtaken by a newer `open` or a `close` fails with `CANCELLED`. `read`: `'auto'` (FileReaderSync for files, `fetch` for URLs), `'sync'` or `'async'` |
| `{type: 'close'}` | `STATE` = no disc; requests fail `NO_DISC` |
| `{type: 'check'}` | reads the first sector now (5 s at most); ok, or the error (and state `error`) |
| `{type: 'stop'}` | as above; afterwards the page may `terminate()` the worker |
| `{type: 'status'}` | only the reply |

## 5. Phase 2: the C++ side and the interface

Built on 2026-10-16 for flat images (5.1–5.3, plus the page); 5.4 is not built. The plan as it
was written follows; 5.6 records the differences and the results. Desktop builds do not change
(`#ifdef __EMSCRIPTEN__`, and the new files only build for Emscripten).

### 5.1 `pcsx2/CDVD/Emscripten/WebDisc.{h,cpp}`: the request-block client

```cpp
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include "common/Pcsx2Defs.h"
#include <atomic>
#include <cstdio>
#include <string_view>
class Error;

namespace WebDisc
{
	static constexpr u32 MAGIC = 0x4b534454; // "TDSK"
	static constexpr u32 VERSION = 1;
	static constexpr u32 SLOTS = 4;
	static constexpr u32 MAX_REQUEST = 16 * 1024 * 1024;

	enum BlockState : s32 { BLOCK_DETACHED = 0, BLOCK_NO_DISC = 1, BLOCK_READY = 2, BLOCK_STOPPED = 3 };
	enum SlotState : s32 { SLOT_FREE = 0, SLOT_CLAIMED = 1, SLOT_PENDING = 2, SLOT_BUSY = 3, SLOT_DONE = 4 };
	enum Result : s32 { E_IO = -1, E_INVALID = -2, E_NO_DISC = -3, E_STALE = -4, E_CANCELLED = -5,
		E_NETWORK = -6, E_TIMEOUT = -7 };

	struct Slot
	{
		std::atomic<s32> state, result;
		std::atomic<u32> dst, length, offset_lo, offset_hi, generation;
		std::atomic<s32> cancel;
	};

	struct alignas(64) RequestBlock
	{
		std::atomic<u32> magic{MAGIC}, version{VERSION}, slots{SLOTS}, slot_bytes{sizeof(Slot)};
		std::atomic<s32> state{0}, doorbell{0};
		std::atomic<u32> generation{0}, size_lo{0}, size_hi{0}, max_request{0};
		std::atomic<s32> error{0};
		u32 reserved[5] = {};
		Slot slot[SLOTS] = {};
	};
	static_assert(sizeof(Slot) == 32 && offsetof(RequestBlock, slot) == 64);

	/// The block (static storage, so ready before any code runs). Its address is what
	/// pcsx2web_disc_block() returns to the page.
	RequestBlock* GetBlock();

	/// Disc paths the page owns: "/disc/<name>" (the page writes a 0-byte placeholder there so
	/// FileSystem::FileExists() and VMManager's checks pass).
	bool IsDiscPath(std::string_view path);

	struct Info
	{
		u32 generation;
		u64 size;
	};
	/// Reads generation, state and size consistently (generation read before and after, like a
	/// seqlock). False with an error when the page has no disc open.
	bool Query(Info* info, Error* error);

	/// Reads length bytes at offset into dst. Blocks the calling pthread (never the browser's main
	/// thread). Returns the bytes read, fewer only at the end of the disc, or a Result code.
	/// `cancel`, polled every 10 ms while waiting, cancels the request (withdraw or CANCEL word).
	s32 Read(u32 generation, void* dst, u64 offset, u32 length, const std::atomic<bool>* cancel = nullptr);

	/// A read-only, unbuffered FILE* over the disc (fopencookie), for the CHD/CSO/GZ readers.
	std::FILE* OpenCFile(const char* path, Error* error);
} // namespace WebDisc
```

Notes for `WebDisc.cpp`:

- **Waiting.** Use `emscripten_futex_wait(&slot.state, s, ms)` and
  `emscripten_futex_wake(&block.doorbell, 1)` from `<emscripten/threading.h>`; these are
  `memory.atomic.wait32`/`notify` on exactly that word.
  - Do **not** use `std::atomic::wait`/`notify`: libc++ may wait on a contention-table entry
    instead of the word itself, so a notify from JS would not wake it.
  - `requester.c` uses `__builtin_wasm_memory_atomic_wait32` directly, and that is what the
    harness tested.
- **The read sequence** (`requester.c` is the reference):
  1. Return `E_CANCELLED` or `E_NO_DISC` at once unless `STATE` is ready.
  2. Claim a slot with `CAS FREE→CLAIMED`. If every slot is taken, sleep 1 ms and retry.
  3. Fill the fields, store `PENDING`, add 1 to the doorbell and wake one waiter.
  4. Wait on `slot.state` in slices of 500 ms (10 ms when `cancel` is set). After each timeout,
     if `STATE` is not ready or a cancel was asked, try `CAS PENDING→FREE`: if it wins, return
     `E_NO_DISC` or `E_CANCELLED`. If the slot is `BUSY`, set `CANCEL` (when cancelling), ring the
     doorbell and keep waiting; the worker always finishes busy requests.
  5. Read `RESULT`, store `FREE` and return.
- `pxAssert(!emscripten_is_main_browser_thread())`. Under `PROXY_TO_PTHREAD` no C++ runs there.
- **The cookie FILE.** `fopencookie` comes from musl, in Emscripten's libc
  (`system/lib/libc/musl/src/stdio/fopencookie.c`).
  - The cookie holds `{generation, size, pos}`. `read` calls `Read()` and advances `pos`. `seek`
    handles `SEEK_SET`, `SEEK_CUR` and `SEEK_END` against `size`, so
    `FileSystem::FSize64`/`FSeek64` work unchanged. `write` is null.
  - **Call `setvbuf(fp, nullptr, _IONBF, 0)`.** With a buffer, musl's `cookieread` splits every
    `fread` into two callbacks: `len - 1` bytes into the caller's buffer, then a refill of the
    1 KiB stdio buffer. That doubles the round trips. Unbuffered, an `fread` makes exactly one
    callback, straight into the caller's buffer.
- **The export.** In `pcsx2-wasm/Main.cpp` (owned by the frontend session), add
  `pcsx2web_disc_block()`, which returns `(u32)WebDisc::GetBlock()`, and add it to
  `PCSX2_WASM_EXPORTS`. It touches nothing and never blocks.

### 5.2 `pcsx2/CDVD/Emscripten/WebDiscFileReader.{h,cpp}`: flat images

```cpp
class WebDiscFileReader final : public ThreadedFileReader
{
	DeclareNoncopyableObject(WebDiscFileReader);
	u32 m_generation = 0;
	u64 m_size = 0;

public:
	WebDiscFileReader();
	~WebDiscFileReader() override;

	bool Open2(std::string filename, Error* error) override; // m_filename = filename; WebDisc::Query()
	Chunk ChunkForOffset(u64 offset) override;                // 128 KiB chunks, as FlatFileReader
	int ReadChunk(void* dst, s64 chunkID) override;           // WebDisc::Read(); < 0 -> -1
	void Close2() override {}                                 // the page owns the file
	u32 GetBlockCount() const override { return static_cast<u32>(m_size / m_blocksize); }
	// Precache2: the base class's "not supported" (the disc does not fit in memory)
};
```

- **Chunk size.** Keep 128 KiB. Measured per-chunk cost from a File is 0.7–0.9 ms; with 1 MiB
  chunks throughput doubles but each readahead miss would cost 2.3–2.9 ms.
- **Generation.** `m_generation` is fixed at `Open2`. After a disc swap (a new `open` in the
  worker), reads return `E_STALE` until PCSX2 reopens the disc, which the swap path does anyway.
- **Cancellation.** `ThreadedFileReader`'s `m_requestCancelled` is checked between chunks, and a
  128 KiB file read takes under 1 ms, so `ReadChunk` passes no cancel token. URL sources could
  pass one later.

### 5.3 How `GetFileReader` picks it

In `pcsx2/CDVD/InputIsoFile.cpp`:

```cpp
static std::unique_ptr<ThreadedFileReader> GetFileReader(const std::string& path)
{
	const std::string_view extension = Path::GetExtension(path);
	// chd / cso / zso / gz / dump: unchanged (their FILE* comes from WebDisc::OpenCFile, 5.4)
	...
#ifdef __EMSCRIPTEN__
	if (WebDisc::IsDiscPath(path))
		return std::make_unique<WebDiscFileReader>();
#endif
	return std::make_unique<FlatFileReader>();
}
```

**Page flow.**
1. At start-up the page reads `_pcsx2web_disc_block()`, creates the worker and posts `attach`.
2. When the user picks a disc, the page posts `open`.
3. On `ready`, it writes the 0-byte placeholder `/disc/<name>` with `FS.writeFile`.
4. It boots with that path; a boot export taking a disc path is a frontend change.

The disc worker lives as long as the page. `stop` is sent only when the page shuts the runtime
down.

### 5.4 Compressed formats on top of the byte-range reader

- CHD, CSO/ZSO and GZ already do all their I/O through a `std::FILE*`:
  - `ChdFileReader` wraps it in a `core_file` (`ChdFileReader.cpp:373`).
  - `CsoFileReader` uses `fread` and `FSeek64` (`:71`).
  - `GzippedFileReader` passes it to `zlib_indexed`'s `extract()` (`:186`).
- So one unbuffered cookie `FILE*` over `WebDisc::Read` serves all three unchanged. The only edit
  is at their three open calls, under `#ifdef __EMSCRIPTEN__`:
  `if (WebDisc::IsDiscPath(path)) fp = WebDisc::OpenCFile(path, error);`. For CHD, wrap the
  result in `FileSystem::ManagedCFilePtr`.
- **Alternative.** Hook `FileSystem::OpenCFile` itself. `common/` cannot depend on `pcsx2/`, so it
  would need a registration hook, and it would also catch `FlatFileReader`. The three explicit
  call sites are clearer.
- **Cost.** CSO/ZSO frames (usually 2 KiB of output, smaller compressed) and CHD hunks (2–19 KiB)
  make small reads. A small read costs as much as a 64 KiB one (0.2–0.5 ms against 0.3–0.6 ms).
  If compressed images stream too slowly, add a read-through cache in `WebDisc`: two or four
  aligned 128 KiB windows per FILE. That cuts the round trips per byte by about 30 times for
  sequential access. Measure first.
- **GZ.** The first open scans the whole file to build its access index (`build_index`) and
  writes `$(f).pindex.tmp` next to the image. On the web, `GzipIsoIndexTemplate` must point to a
  writable directory (MEMFS or OPFS `/data`). For a 3 GB image the scan reads everything through
  the worker (10–25 s at the measured rates) plus the inflate time.
- **CHD parents.** `ChdFileReader` looks for parent CHDs in the image's directory with
  `FindFiles` (`:312`). A single picked file has no directory, so parent CHDs are unsupported
  until the page lets the user pick several files. That needs one block or source per file (a
  `FILE_ID` slot word in version 2), or one worker per file.

### 5.5 Checks for Phase 2

- Boot the test disc in the browser through `serve.py --disc` (URL) and through a picked File.
- `IsoHasher` or a full read must match `sha256sum` of the image.
- Test a CSO and a CHD made with PCSX2's tools from a disc the owner owns.
- Swap discs while the game runs.
- Stop the VM during reads.
- Measure the time spent waiting in `FinishRead` during FMV or streaming.

### 5.6 What was built, and what is open

**Built.**
- `pcsx2/CDVD/Emscripten/WebDisc.{h,cpp}`: the block, `IsDiscPath`, `Query`, `Read`, plus
  `AbortReads`/`ResumeReads` and `ResultName`. No `OpenCFile` (5.4 is not built).
- `pcsx2/CDVD/Emscripten/WebDiscFileReader.{h,cpp}`: as 5.2.
- `GetFileReader()` (`InputIsoFile.cpp`): `/disc/` paths get a `WebDiscFileReader` before the
  extension checks, so every disc path is read as a flat image. The page refuses `.chd`, `.cso`,
  `.zso`, `.gz` and `.dump` files with their own message, and any file without an ISO 9660 volume
  at one of the layouts `InputIsoFile::Detect()` tries.
- `pcsx2-wasm/Main.cpp`: `pcsx2web_disc_block()`, `pcsx2web_boot_disc(bios, path, flags)` (fast
  boot unless `flags` is 1) and `pcsx2web_game()` (title and serial from `Host::OnGameChanged`, as
  a seqlock). `pcsx2web_stop()` calls `WebDisc::AbortReads()`.
- The page (`app.js`, "discs" section; `UI-SPEC.md` §10): the worker starts at `Module.postRun`;
  picker (`showOpenFilePicker` where available, else `<input type=file>`), drag and drop, `?disc=`,
  the File System Access handle kept in IndexedDB (`pcsx2-web-disc`) and reopened with one press,
  the placeholder at `/disc/<name>`, errors, and the game's title.

**Different from the plan, after the review of Phase 1.**
- *A destination is never released while its slot is busy.* `Read` returns only after `DONE`, or
  after withdrawing a request that is still `PENDING`; for a busy one it sets `CANCEL` and keeps
  waiting. The worker finishes busy requests on cancel, close and stop.
- *Reads cannot hold the CPU thread for long.* `ThreadedFileReader::ReadSync()` calls `ReadChunk()`
  on the CPU thread with the reader's mutex held, and a URL read could take three 30 s attempts.
  `Read` cancels after `READ_TIMEOUT_MS` (10 s; `CANCEL` = 2, answered `TIMEOUT`, which
  `ReadChunk` logs) and when `AbortReads()` was called (every Stop), which also wakes the waiting
  slots at once. The timeout makes the source unavailable in the worker (4.2), so a stalled server
  costs one 10 s wait, not one per PCSX2 retry (up to 256). `pcsx2web_boot_disc()` calls
  `ResumeReads()` before it asks for the boot, on the page's thread like `pcsx2web_stop()`, so a Stop
  right after a boot always cancels that boot's reads. A Stop during disc reads took 54 ms in the
  browser test.
- *`Query` waits out a publish.* Publishing runs on the worker's thread, so `Query` can see its
  middle ("no disc"); it answers "no disc" only once the state has stayed so for 20 ms.
- *If the worker's loop dies* it hands back what it had taken and reports `stopped` (4.2). The
  first version stopped the block and failed everything instead, so that nothing would wait for a
  worker that was gone; but PCSX2 runs through its 256 retries far faster than a worker restarts,
  so the game would get a read error from a recoverable crash. The block now stays ready while the
  page restarts the worker; if nobody serves it, each request still ends at its 10 s deadline.
- *If the worker itself dies* (an `error` event, or `stopped` from a dead loop), the page terminates
  it and waits until it is granted the worker's life lock: only then is the worker known to be gone
  (without Web Locks, the page waits 1 s, which is safe for the `File` and URL sources it sends,
  whose bytes reach the memory only through the worker's JS; it never sends OPFS handles). It then
  hands back the busy slots (`BUSY`→`PENDING`) and starts a worker that resumes the current disc
  under the same generation, at most three times; a running game goes on reading its disc. After
  the third restart the page stops the block and finishes pending and busy slots with `CANCELLED`
  (CAS `PENDING`→`BUSY` first, so a requester withdrawing at the same moment loses cleanly). A
  resume that fails leaves the game without its disc; the page then says so and opens the disc
  again for the next boot.
- *A failed disc swap keeps the old disc readable*: `open` no longer drops the current disc first
  (4.3). The page offers no swap while a game runs anyway, so PCSX2's own `ChangeDisc()` revert
  path is never asked to reopen a stale generation; that needs a source per generation (v2) before
  swap can be offered.
- *An open overtaken by a newer open or a close replies `ok: false` (`CANCELLED`)*, so the page
  never takes it for the current disc.
- *Publishing is one synchronous step*: `STATE` = no disc, generation + 1, size, generation + 1,
  `STATE` = ready, which `Query` reads as a seqlock.

**Checks (5.5).**
- The test disc boots in the browser through `serve.py --disc` (URL source): language menu, title,
  menus, character select and a battle, with keyboard input; Stop and Boot again; the save made in
  the game survives a reload (log `McdSlot 0 ... Formatted`, the save directory `BESLES-54945DBZT3`
  in the IndexedDB copy, and the title screen defaults to Continue, which goes straight to the main
  menu). Reads over a session: 889 requests, 116 MB, 5 ms average per 128 KiB through `serve.py`.
- File sources were tested with `File`s built in the page (drops) and with OPFS files reopened
  through a stored handle; the pane cannot open a real file picker. A file changed after it was
  opened (an OPFS file rewritten in place) is reported: "The disc can't be read any more", and the
  slate's error after Stop.
- The harness passes 59 checks in each of its 4 configurations in the pane (Chrome 152), including
  `failed-open-keeps-disc`, `superseded-open-fails`, and, after the second review:
  `deadline-cancel-times-out`, `unavailable-fails-fast`, `unavailable-recovers` (about 1 s),
  `check-ok`, `check-file-gone`, `life-lock-held`, `life-lock-released-on-terminate` (0.4–1.6 ms),
  `resume-keeps-generation`, `resume-serves-handed-back-read` (a 16 MiB URL read taken by a worker
  that was then terminated, served by the next one, hash checked) and `resume-mismatch-has-no-disc`.
- The life lock, measured: a worker terminated 1–20 ms into an 860 ms synchronous OPFS read into a
  `SharedArrayBuffer` went on writing for about 800 ms; its lock was granted only after the write
  ended (805–889 ms), with nothing written afterwards. A fixed wait after `terminate()` is not enough
  for such sources.
- In the browser with the game (URL source): a stopped server froze the game once for 10 s, then
  PCSX2's 256 retries failed at once and the emulator kept running, with the subtitle; reads worked
  again about 1 s after the server resumed. A disc worker crashed (or its loop killed) while a read
  was in flight: the read was handed back, the next worker took the disc over under the same
  generation, and the game went on from the language menu to the logos and the intro movie. After
  the third restart, reads failed at once and the slate said so; Stop took 62 ms.
- Not done: a full-image hash through the C++ reader, CSO and CHD, disc swap, and measuring the time
  the CPU thread waits in `FinishRead` during movies.

**Open.**
- 5.4 (CHD, CSO/ZSO, GZ through a cookie `FILE*`), blockdump images, and multi-file images.
- Disc swap and eject while running (needs one source per generation, see above).
- A worker stuck inside a synchronous read (a hung network drive) still holds its requester; the
  page cannot tell it from a slow read. Reads time out only on the C++ side, which then waits for
  the worker to finish the busy slot, and a restart would wait for the stuck worker's lock.

## 6. Page UX notes

These are for `app.js` and `UI-SPEC.md`, which their owners decide; this note does not change
them.

- **Picking.**
  - Use `<input type="file" accept=".iso,.bin,.img,.chd,.cso,.zso,.gz">` and post
    `{type: 'open', file}`.
  - Drag and drop: use `event.dataTransfer.files[0]`. In Chromium,
    `DataTransferItem.getAsFileSystemHandle()` also gives a handle the page can remember.
  - The file never leaves the device; say so next to the picker, as the BIOS screen does.
- **Reopening after a reload, with the File System Access API.**
  - `showOpenFilePicker()` exists in Chromium only (Chrome 152 and 155: `function`; Firefox 157:
    `undefined`).
  - Store the `FileSystemFileHandle` in IndexedDB (handles are structured-cloneable).
  - On the next visit, `handle.queryPermission({mode: 'read'})` usually answers `'prompt'`. Show a
    "Continue with <name>" button: `requestPermission` needs a user gesture.
  - After the grant, post `{type: 'open', handle}`. The worker calls `getFile()` itself and
    reports `NotAllowedError` as "Permission to read the disc file was not granted."
- **Without the File System Access API** (Firefox, Safari): the user picks the file again after
  each reload. Remember name, size and `lastModified`, and the disc serial once known, so the
  page can say "Pick <name> again" and recognise the same disc.
- **States.**
  - `opening`: show progress. Opening a File is immediate; a URL needs one request.
  - `ready`: allow Boot.
  - `error`: show `status.error.message` and offer "Pick the file again".
  - `idle` after an `open` that failed: show the error from the reply.
- **Disc-activity light.** Poll `stats.reads`, or watch the `{type: 'status'}` messages, if the
  spec wants one. Never poll faster than the UI needs.
- **Optional "keep on this device" (OPFS).**
  - Only where the copy works and fits. Check `navigator.storage.estimate()` and call
    `navigator.storage.persist()` first.
  - Show the copy progress, check every `write()` return value and the final size (the Chrome 152
    trap), and keep a way to delete the copy.
  - Never make the copy the default.

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Slow storage (HDD, USB, network shares): cold random reads of 5–15 ms | Medium | Stutter in streaming games | Readahead in `ThreadedFileReader`. A larger or longer readahead on wasm if `FinishRead` waits show up. These numbers are warm-cache. |
| `terminate()` without `stop` while a request is `BUSY`: the requester waits for ever | Low | Hung CPU thread | On a worker `error` event (or a worker that stopped by itself) the page terminates it, waits for its life lock, hands its busy slots to a new worker that resumes the disc, and after three restarts stores `STATE = stopped` and fails pending and busy slots (built, 5.6). C++ waits in slices, re-checks `STATE`, and after 10 s withdraws a pending request or asks the worker to cancel a busy one. |
| File changed in place at the same size | Low | Wrong data, no error | Picked files are snapshots whose modification time browsers are expected to check (not measured here). OPFS same-size rewrites are not detected (measured). Remember size and `lastModified`, and recheck sector 16 on reopen. |
| `Atomics.waitAsync` missing | Low (Chrome 152/155 and Firefox 157 have it; Safari 16.4+ per its documentation) | Up to 50 ms latency for page messages only | Blocking fallback, tested with the full suite. |
| Electron/Chrome 152 storage limits (OPFS file cap of 2000 MiB, large Blob fetch fails) | Certain in the pane | OPFS option unusable there | The main path does not use them. Re-measure before offering the OPFS option. |
| Safari not tested (none available here) | — | Unknown latency, iOS file sizes | Test on Safari before release. FileReaderSync and SAB under COOP/COEP are supported there. |
| Small reads from compressed images | Medium | Slower CSO/CHD streaming | A read-through window cache in `WebDisc` (5.4), after measuring. |
| GZ index scan and its index file | Medium | Long first open; write to a read-only path | `GzipIsoIndexTemplate` to `/data`. Show progress. |
| Several files per disc (CHD parents, multi-track BIN/CUE) | Medium | Not supported in v1 | Version 2 adds a file id per slot, or one worker per file. |
| The dev server's `--disc` used to share games | Low | Legal | It binds to 127.0.0.1 only, checks Host, serves one named file read-only, and is documented as dev-only. |

## 8. Files and how to test

**Files.**
- `pcsx2-wasm/web/disc-worker.js`: the worker (the deliverable of Phase 1).
- `tools/wasm/serve.py`:
  - single byte ranges on every file (206 with `Content-Range` and `Content-Length`; 416 with
    `bytes */size`). Multiple ranges, other units and malformed headers are ignored with a 200.
    `If-Range` is checked against `Last-Modified`.
  - `--disc FILE`, which serves exactly that one file read-only as `/dev-disc/<its name>`.
  - `TCP_NODELAY`.
  - Everything it did before is unchanged.
- `tools/wasm/disc-harness/`:
  - `index.html` and `harness.js`: the page and its tests.
  - `requester.js`: a JS requester.
  - `requester.c` and `build-requester.sh`: the C requester, which builds `requester.wasm`
    (ignored by git).
  - `opfs-tool.js`: OPFS import, write speed and changed-file helpers.

**Running it.**

```sh
python3 tools/wasm/serve.py --port 8110 --disc /path/to/disc.iso .   # repository root as web root
# open http://127.0.0.1:8110/tools/wasm/disc-harness/index.html?disc=/dev-disc/<url-encoded name>
# Run tests (about 4 s), Run benchmarks; "Copy the disc URL into OPFS" adds the OPFS sources;
# the file input runs the File tests on a file you pick. From the console:
#   await discHarness.runCore({impl: 'wasm', loop: 'blocking'})   // after build-requester.sh
```

Results are in the page's table and in `window.discHarness.results`. Every run removes its OPFS
copy with "Remove the OPFS copy" (`discHarness.removeOpfsCopy()`). `serve.py` was tested with
79 curl checks:
- headers and refusals;
- ranges at the start, middle, odd offsets and the last bytes of the 3.2 GB image, each compared
  with `dd`;
- clamping, suffix ranges, 416s and ignored forms;
- `If-Range`, HEAD, keep-alive;
- path traversal on `/dev-disc/`, `/dev-bios/` and the web root, including encoded `..`,
  backslashes, NUL, symlinks and dot files;
- Host checks, and 501 for POST/PUT/DELETE.
