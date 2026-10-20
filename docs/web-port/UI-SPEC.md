> **Product name (2026-10-16):** the owner named the app **TabStationToo**. It replaces the working title "PCSX2 Web" everywhere user-facing. Wordmark: "TabStation" as the main part, "Too" as the secondary part. Credits keep "Built on PCSX2 (GPL-3.0)" and "not affiliated with Sony or the PCSX2 project".

## 1. Concept

**House Lights.** The page works like a small cinema. With the lights up (setup, stopped, failed), the room is a warm brown-black and the unlit screen is a matte slate. Every message is a flush-left title card on that screen. When the first PS2 frame arrives, the room goes black, the controls fade, and the picture becomes the only light. Then an amber **tally** square under the screen's lower-right corner switches on. The screen and its tally together are the logo at full scale, so the identity survives a rename. **Hushed, exact, warm.**

What it avoids:
- cards, panels, pills, chips and pulsing dots
- navy and electric blue: the only accent is amber, and it means only "where your input goes"
- glows, gradients, blur, grain and shadows
- the centred hero with an icon above the title: titles sit flush left, in sentence case, and end with a full stop
- uppercase micro-labels, the EXPERIMENTAL chip, helper paragraphs and toasts (subtitles replace toasts)

The signature is the type: Archivo's width axis used at its extremes (125 for titles, 70 for units), with Chivo Mono for every number.

## 2. Information architecture and flows

The page is one column, top to bottom:
- **marquee:** the mark, the wordmark (`h1`) and About
- **screen**, with the **slate** laid over it
- **sill:** capture state, subtitle, readout and tally
- **transport:** the controls
- **drawer:** Keys or Log
- **credits:** the footer

There is also a BIOS-menu popover and an About dialog. Before play, the slate holds the single primary action. During play, the transport does.

Space reserved for later, with no controls today:
- memory-card and save-state slots, left of the disc menu (the disc slot is in use, see §10)
- volume, first on the right
- Settings, as a third drawer
- a Gamepad tab in Keys

**Flows:**
- **First run:** the runtime starts downloading on load while the slate asks for a BIOS. Neither waits for the other.
- **BIOS chosen, or returning user:** the title card appears. It says "This visit only" until IndexedDB confirms the save. Nothing ever boots by itself.
- **Boot pressed before the download finishes:** the boot is queued (`page.autoBoot`). Pressing again cancels it. `?bios=` starts in this state; cancelled while its file is still downloading, it shows that file's title card (without the size until it arrives).
- **Starting:** elapsed seconds, then "waiting for the first frame". A reassuring line appears at 10 s and a link to the log at 15 s.
- **Running:** the lights go down and the frame line and tally turn amber. The controls hide after 2.5 s idle. One hint shows on a browser's first boot.
- **Released** (by Esc, Tab, window blur or a hidden tab): the frame line and tally go neutral, the controls stay visible, and "Click the screen to play" appears.
- **Stopped:** the last frame stays on screen, dimmed.
- **Failed:** a log excerpt.
- **Fatal:** Reload and Copy details. Only Log stays in the transport.
- **Unsupported:** a self-test, and nothing downloads.
- **Touch-only devices** (`(pointer: coarse) and (hover: none)`): the download waits for a BIOS, and the controls never hide.
- **Under 640 px:** the slate sits below the screen and the transport takes two rows.

## 3. Screens

At 1440×900: marquee 48 px, sill 32 px, transport 48 px, credits 32 px, gutter 24 px. The screen is about 976×732, and the sill and transport match its width.

**D1. First run, downloading**
```
 ▣ TabStationToo                                              About
  ┌───────────────────────────────────────────────────────┐
  │   Start with your BIOS.                               │
  │   TabStationToo needs the BIOS from a PS2 you own, …      │
  │   [ Choose BIOS file… ]  or drop it here              │
  │   It stays in this browser. Nothing is uploaded.      │
  │   How to dump your BIOS ↗                             │
  ├━━━━━━━━━━━━━━━━━━━━━━━━━──────────────────────────────┤ amber hairline
  Preparing the emulator  6.1 / 14.2 MB
                                 ▦ Crisp  ⌨ Keys  >_ Log  ⛶ Fullscreen
     Use a BIOS from your own console; it never leaves this browser …
```
**D2. Ready**
```
  │   SCPH-39001                                          │
  │   SCPH-39001.bin · 4 MB · Remembered in this browser  │
  │   [▶ Boot  Enter]   Use another BIOS…                 │
  [▣ SCPH-39001 ▴]                 ▦ Crisp ⌨ Keys >_ Log ⛶ Fullscreen
```
Starting looks the same, plus Stop, and the slate reads "Starting the PS2…  4 s".

**D3. Running.** When the lights are down, only the picture, the frame line, the speed and the tally remain.
```
  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓ 2px amber
  ┃                PS2 picture, 4:3                       ┃ + 2px gap
  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
  Keys go to the PS2              98% speed  59.9 VPS  60 FPS  ■
  [■ Stop] [▣ SCPH-39001 ▴]       ▦ Crisp ⌨ Keys >_ Log ⛶ Fullscreen
```
**D4. Keyboard released, Keys drawer open** (the screen resizes once)
```
  Click the screen to play        98% speed  59.9 VPS  60 FPS  □
  ─────────────────────────────────────────────────────────────
  L2 [1]  L1 [Q]       Select      Start        R1 [E]  R2 [3]
      [↑]            [Backspace]  [Enter]           △ [I]
  [←]     [→]                                   □ [J]     ○ [L]
      [↓]                                           ✕ [K]
  Left stick [W][A][S][D]  L3 [2]    R3 [4]  Right stick [T][F][G][H]
  note · shortcuts                [ ] Keep controls visible     ×
```
**D5. Stopped:** the dimmed frame, with "Stopped.", then `SCPH-39001.bin · ran 04:12`, then `[▶ Boot again  Enter]`.

**D6. Failed:** the title, one line of text, a 6-line mono log excerpt on `--raise`, and `[↻ Try again]  Use another BIOS…  Open the log`. The Log drawer header reads `Log  1,204 lines … Jump to latest  Copy  Clear  ×`.

**D7. Unsupported:** the cause line, then "Details for developers". The disclosure holds the Pass/Fail rows, the header lines and the `serve.py` command with Copy.

**P1. Phone (390×844)**
```
┌────────────────────────────────────┐
│ ▣ TabStationToo                  About │
│ ┌────────────────────────────────┐ │
│ │      unlit screen 358×268      │ │
│ └────────────────────────────────┘ │
│ Start with your BIOS.              │
│ [       Choose BIOS file…        ] │
│ The emulator (14 MB) downloads …   │
│   ▦       ⌨       >_       ⛶       │
│ Crisp    Keys    Log   Fullscreen  │
└────────────────────────────────────┘
```
While running on a phone, the first transport row has Stop and the BIOS menu, and the second row has the tools.

## 4. Visual system

**Colour.** Dark only. Contrast figures are WCAG ratios.

| Token | Hex | Use, contrast |
|---|---|---|
| `--house` | #121110 | room, lights up |
| `--house-down` | #000000 | room, running |
| `--slate` | #1C1B19 | unlit screen |
| `--raise` | #262421 | key caps, popover, log excerpt, dialog |
| `--line` | #2E2C29 | decorative lines; frame line when the keyboard is released |
| `--line-2` | #726D65 | essential edges: 3.7 on house, 3.0 on raise |
| `--ink` | #ECE8E1 | text: 15.5 on house, 12.7 on raise |
| `--ink-2` | #ABA59C | secondary text, icons: 7.7 / 6.3 |
| `--ink-3` | #968F86 | fine print, units: 5.9 / 4.8 |
| `--amber` | #F2B544 | accent: 10.3 on house, 11.5 on black; hover #F7C566, pressed #E0A537 |
| `--amber-ink` | #1A1306 | text on amber: 10.1 |
| `--coral` | #EE7B62 | errors: 6.9 |
| `--sand` | #D9C08A | log warnings |

On a dimmed frame (worst case #333), ink, ink-2 and coral stay at 4.6:1 or better, but ink-3 drops to 3.95:1, so ink-3 is never used there. Face glyphs are always `--ink`.

**Type.** Both faces are by Omnibus-Type, under the SIL OFL 1.1. Fallbacks are `ui-sans-serif` and `ui-monospace`.

| Role | Face, size/line | Weight | Width | Tracking |
|---|---|---|---|---|
| Wordmark | Archivo 16/20 | 640 ("Web" 400, ink-2) | 125 | +0.01em |
| Slate title | Archivo clamp(26px, 2.4vw, 36px)/1.08 | 500 | 125 | −0.02em |
| Slate body, max 44ch | Archivo 16/24 | 400 | 100 | 0 |
| Buttons; sill and labels | Archivo 14/20; 13/18 | 560; 480 | 100 | 0 |
| Fine print, credits | Archivo 12.5/18 | 400 | 100 | 0 |
| Units, keymap labels | Archivo 11/16 | 540 | 70 | +0.03em |
| Numbers, names, key caps, log | Chivo Mono 12/16 (log 12/18) | 600 speed, 500 caps, 450 | | 0 |

The Latin subsets have no ← → ↗ ▪ ▫ and no face-button shapes, so draw those in SVG or CSS. The subsets do include … · × and curly quotes.

**Spacing and shape.**
- Spacing is on a 4 px base: 4, 8, 12, 16, 24, 32, 48, 64.
- The gutter is 24 px, or 16 px under 640 px.
- Slate text starts 1/12 of the screen width in from the left, is centred vertically, and is at most 30rem wide.
- Hit targets are 40 px, or 44 px on coarse pointers.
- Radius is 0, except 3 px on buttons and key caps.
- There are no shadows. Depth comes from brightness steps and 1 px lines. The popover gets a 1 px `--line-2` border plus a 1 px black ring.

**Frame line (the capture signal).** An `outline` on `.screen`, offset 2 px so a strip of room colour separates it from the picture. It is 2 px full amber when the keyboard is captured, on `:focus-visible`, and during drag-over. It is 1 px `--line` when the keyboard is released.

**Tally.** An 8×8 square at the right end of the sill. When captured it is `border: 4px solid var(--amber)`; otherwise it is hollow, with a 1 px `--line-2` border.

**Mark.** A 16×12 ink outline (1.5 px) with a 4×4 amber square inset 2 px at the lower right, sized to the wordmark's cap height. `favicon.svg` is the mark on #121110.

**Icons.** Phosphor Regular (MIT), at 20 px, in `--ink-2` (`--ink` on hover). Pressed toggles add a 2 px `--ink` bar under the icon.
- Set: play, stop, memory, caret-up, folder-open, trash, checkerboard, keyboard, terminal-window, corners-out, corners-in, copy, x, arrow-clockwise, arrow-up-right, arrow-line-down, and arrow-up/down/left/right.
- The face-button shapes are drawn in-house on Phosphor's 256 grid with a 16-unit stroke.
- No texture.

**ASSET MANIFEST.** Sizes were checked. Fontsource's `wdth.css` declares the Archivo file as `font-weight: 100 900; font-stretch: 62% 125%`.

| Asset | Files in `web/` | Source; download | Licence | Size |
|---|---|---|---|---|
| Archivo variable, Latin | `fonts/archivo.woff2`, `fonts/Archivo-OFL.txt` | github.com/Omnibus-Type/Archivo; fontsource.org/fonts/archivo; https://cdn.jsdelivr.net/npm/@fontsource-variable/archivo@5.3.0/files/archivo-latin-wdth-normal.woff2; https://raw.githubusercontent.com/google/fonts/main/ofl/archivo/OFL.txt | OFL 1.1 | 90,104 B |
| Chivo Mono variable, Latin | `fonts/chivo-mono.woff2`, `fonts/ChivoMono-OFL.txt` | github.com/Omnibus-Type/Chivo; fontsource.org/fonts/chivo-mono; https://cdn.jsdelivr.net/npm/@fontsource-variable/chivo-mono@5.3.0/files/chivo-mono-latin-wght-normal.woff2; https://raw.githubusercontent.com/google/fonts/main/ofl/chivomono/OFL.txt | OFL 1.1 | 26,336 B |
| Phosphor core 2.1.1 Regular, 20 icons | `icons.svg` symbols, `icons/Phosphor-LICENSE.txt` | github.com/phosphor-icons/core; https://cdn.jsdelivr.net/npm/@phosphor-icons/core@2.1.1/assets/regular/NAME.svg (plus `LICENSE` at that package root) | MIT | 6.5 KB |
| Face glyphs, mark | `icons.svg`, `favicon.svg` | in-house | GPL-3.0 | <1 KB |

The total is about 124 KB.
- Load the fonts with `format("woff2")` and `font-display: swap`, and preload Archivo only.
- Add `".woff2": "font/woff2"` and `".txt": "text/plain; charset=utf-8"` to `MIME_TYPES` in `tools/wasm/serve.py`.
- The Roboto, Font Awesome and promptfont files already in the repo stay unused.

## 5. Components and states

- **Screen** (`role="application"`, `tabindex="0"`). States: unlit, picture, dimmed (`brightness(.2) saturate(.3)` when stopped or failed), drop target, captured, released.
- **Slate** (`section`, `h2[tabindex=-1]`, no aria-live). Variants: intake, title card, queued, starting, stopped, failed, fatal, unsupported, drop. It can show a coral error line, which stays until the next attempt.
- **Primary button.** Amber, 40 px tall, 3 px radius. It carries an 11 px mono "Enter" key cap when Enter also works from the screen. It appears only on the slate and is never disabled.
- **Quiet buttons and links.** Quiet buttons are ink-2 and turn ink on `--raise` when hovered. Links are underlined with a 3 px offset. External links open in a new tab with `noopener noreferrer`, show the `arrow-up-right` icon, and carry hidden text "(opens in a new tab)".
- **Capture.** While captured, it is plain text that fades with the controls. While released, it is `button#capture`, which focuses the screen.
- **Readout.** Not a live region. It shows "—" while starting, and hidden text adds the frame size. When the lights are down, only the speed stays, in ink-3. The slow-speed hint adds to it and never replaces it.
- **Subtitle** (`role="status"`). The newest replaces the last. It stays 4 s (8 s for the first-boot hint) and pauses on hover. It is only for confirmations and hints.
- **Transport** (`role="group"`).
  - Left: Stop (while starting or running), then the disc menu (while a disc is chosen, §10), then the BIOS menu.
  - Right: Crisp (`aria-pressed`), Keys and Log (`aria-expanded`), Fullscreen.
  - Labels show at 1024 px and wider. Narrower than that, buttons are icons only, with a CSS tooltip on hover and `:focus-visible` that includes the shortcut. On `hover: none` devices the labels sit under the icons.
- **BIOS menu** (popover).
  - The trigger has `aria-expanded` and `aria-controls` and is never disabled. The menu opens upward and focuses its first item.
  - It shows the file name and where it is stored, then "Choose another file…" and "Forget this BIOS".
  - While the PS2 is running, the items are `aria-disabled` and described by the line that gives the reason.
  - Forget asks for confirmation inline, with focus on Keep.
- **Keys drawer.**
  - Keys are laid out like a controller (D4). Caps are at least 28 px, on `--raise` with a 1 px `--line-2` border, and turn amber while held (`kbd.down`).
  - Letter labels come from `navigator.keyboard.getLayoutMap()`, so AZERTY shows Z Q S D; QWERTY is the fallback.
  - It includes "Keep controls visible", saved as `pcsx2web.keepChrome`.
- **Log drawer.**
  - Header: line count, "Jump to latest" (only when scrolled up), Copy, Clear, Close.
  - Body: a `role="log"` area 38vh tall that follows the tail. It has 2 px coral or sand gutter marks and an empty state.
- **Hairline** (`role="progressbar"`, labelled, with `aria-valuetext` such as "6.1 of 14.2 megabytes"). It shows real bytes only, stays full while the runtime initialises, then is removed.
- **About:** a `dialog`, 36rem wide.
- **Announcer:** `p#announce`, visually hidden, `role="status"`, for state changes only.

## 6. Motion

Easings are `--ease-out: cubic-bezier(.2,0,0,1)` and `--ease-in: cubic-bezier(.4,0,1,1)`. Only opacity, colour and filter animate. Nothing loops, and the canvas never moves.

| What | How |
|---|---|
| Lights down | the room fades to black over 900 ms ease-out, then the tally turns on over 160 ms; the lights come back up in 300 ms |
| Controls | hide over 480 ms ease-in; show over 140 ms ease-out, or instantly when focus arrives |
| Slate | crossfade 180 ms; fades out over 240 ms on the first frame |
| Stopped dim | 700 ms |
| Frame line and tally | 160 ms each |
| Hairline | 200 ms linear |
| Drawer | the screen resizes once, and the drawer fades in while rising 8 px over 200 ms; it closes in 120 ms |
| Subtitle | 140 ms in, 320 ms out |
| Key caps | light up instantly, fade 120 ms on release |

With reduced motion, every duration is 0. The controls still hide.

## 7. Microcopy

- **Intake:**
  - "Start with your BIOS."
  - "TabStationToo needs the BIOS from a PS2 you own, usually a 4 MB .bin file. Then add a disc image of one of your games, or boot the console's own menus."
  - "Choose BIOS file…", then "or drop it here"
  - "It stays in this browser. Nothing is uploaded."
  - "How to dump your BIOS" (links to https://pcsx2.net/docs/setup/bios/)
- **Touch-only:** "The emulator (14 MB) downloads after you choose it." / "Made for a computer with a keyboard. Phones may not have the memory to run it."
- **Loading:** "Preparing the emulator 6.1 / 14.2 MB", then "Starting the runtime…"
- **Drop:** "Drop to use this BIOS." While running: "Stop the PS2 first to change the BIOS."
- **Rejected file:**
  - "That file is too small to be a PS2 BIOS (12 KB; a BIOS is usually 4 MB)."
  - "That file is 4.2 GB. A BIOS is 4 to 8 MB." (only for files that are not disc images either; disc images go to §10)
  - "Couldn't read that file. Try choosing it again."
  - "Couldn't fetch the BIOS from /dev-bios/x.bin (HTTP 404)."
  - "?bios= only accepts addresses on this server."
- **Title card:**
  - "SCPH-39001" / "SCPH-39001.bin · 4 MB · Remembered in this browser" (or "This visit only", or "Loaded from ?bios=, not saved")
  - "Boot" · "Use another BIOS…"
  - If the save failed: "Couldn't save the BIOS in this browser. It will last for this visit."
- **Queued:** "Booting when ready · 62%" / "Press again to cancel." (the percentage only while the emulator downloads)
- **Starting:**
  - "Starting the PS2…" / "SCPH-39001.bin · 4 s", then "Waiting for the first frame… 6 s"
  - At 10 s: "Still starting. The first screen can take a few seconds here."
  - At 15 s: "The emulator hasn't answered. The log may say why." with "Open the log"
- **Boot refused:** "The emulator is already starting." / "The emulator refused to boot. The log has details."
- **Capture:** "Keys go to the PS2" / "Click the screen to play" / when the button has focus: "Press Enter to play"
- **Hints:**
  - First boot: "Arrows move · K confirms (L on Japanese consoles) · ? shows every key"
  - Once per visit, after 5 s under 50% speed: "Running at 18%. Heavy scenes are slow in the browser for now."
- **Keys drawer:**
  - "In the system menus ✕ confirms and △ goes back (○ confirms on Japanese consoles)." The glyphs are SVG.
  - "While the PS2 runs, keys you press light up here."
  - "Keep controls visible"
- **Stopped:** "Stopped." / "SCPH-39001.bin · ran 04:12" / "Boot again"
- **Failed:** "The PS2 didn't start." / "PCSX2 couldn't boot SCPH-39001.bin. The end of the log:" / "Try again" · "Use another BIOS…" · "Open the log"
- **Fatal:**
  - "The emulator stopped working." / "Reload to start over. Your BIOS is still saved." (the second sentence only if the BIOS was remembered) / "Reload" · "Copy details"
  - Download failure: "Couldn't download the emulator." / `pcsx2-wasm.wasm: HTTP 404` / "Check the connection to this server, then reload."
  - Build failure: "This build of the emulator is incomplete."
- **Unsupported:** "This page can't run the emulator.", then one of:
  - "The server has to send cross-origin isolation headers, and this one doesn't."
  - "This site needs a service worker to run the emulator, and this browser blocked it. Private windows often do." (on a host without the headers, such as GitHub Pages, when the isolation service worker cannot start; while it installs and reloads the page once, the intake shows as usual and nothing downloads)
  - "You opened the file from disk. Start the dev server instead."
  - "This address isn't a secure context. Use http://127.0.0.1 or HTTPS."
  - "This browser can't run WebAssembly." or "This browser doesn't support shared-memory threads.", followed by "Try a recent Chrome or Edge on a computer."
  - Then "Details for developers", with the rows "WebAssembly", "Shared-memory threads", "Secure context" and "Cross-origin isolated", each marked "Pass" or "Fail".
- **BIOS menu:**
  - "Choose another file…" · "Forget this BIOS"
  - "Stop the PS2 to change the BIOS."
  - "Forget it? You'll need the file to boot again." / "Forget" · "Keep"
  - Afterwards, the subtitle "BIOS forgotten. It's been removed from this browser."
- **Log:** "Log" · "1,204 lines" · "Jump to latest" · "Copy" (shows "Copied" or "Couldn't copy" for 2 s) · "Clear" · "No output yet."
- **Fullscreen:** "Fullscreen" / "Exit fullscreen". If the browser refuses: "Fullscreen isn't available here."
- **Credits:** "Use a BIOS from your own console; it never leaves this browser · Built from PCSX2 (GPL-3.0) · Source · Not affiliated with Sony"
- **About:**
  - "About TabStationToo."
  - "PCSX2, compiled to WebAssembly, running entirely in this tab."
  - "Use only a BIOS dumped from a console you own. It's kept in this browser (IndexedDB), never sent anywhere, and you can forget it from the BIOS menu."
  - "Works today: the BIOS menus and games from disc images, with a keyboard, sound, and a memory card kept in this browser. Most games run well below full speed for now. Not yet: save states, gamepads."
  - "Built from PCSX2, GPL-3.0-or-later. Source"
  - "PlayStation and PS2 are trademarks of Sony Interactive Entertainment. This project isn't affiliated with or endorsed by Sony."
  - "Type: Archivo and Chivo Mono by Omnibus-Type (OFL 1.1). Icons: Phosphor (MIT)." Each name links to its licence file.
  - "Close"
- **Announcer:** "Ready to boot." · "Booting when ready." · "Starting." · "Running. Keys go to the PS2." · "Keyboard released." · "Stopped." · "The PS2 didn't start." · "The emulator stopped working."
- **Screen label:** "PS2 screen. While it runs and this has focus, the keyboard controls the PS2. Escape releases it."

## 8. Interaction details

**Capture.** Keep `captureActive()` and `returnFocusAfterPointer()`, with these changes:
- `captureActive()` returns false when `!document.hasFocus()`.
- Remove its `fs === els.stage` branch. Inside the fullscreen theatre, focus on the screen or on body counts as captured; focus on the sill, transport or drawers counts as released.
- On `blur`, `focus` and `visibilitychange`, re-render the capture state and the controls.
- While running, a pointer-down on the empty room focuses the screen.

**Focus targets.**
- Boot moves focus to the screen. The first frame does too, if focus is on body or on an element that was removed.
- Entering stopped focuses Boot again, and entering failed focuses Try again. Both apply only if focus was on the screen, on body, or on a control that was removed.
- When the user chooses a BIOS, focus Boot. When a BIOS is restored from IndexedDB, focus Boot only if focus is still on body.
- After Forget, focus "Choose BIOS file…".
- Never take focus away from a control the user chose.

**Esc,** in this order:
1. An open dialog or popover closes natively, and focus goes back to the element that opened it.
2. Inside a drawer, Esc closes the drawer and focuses its toggle.
3. With the screen focused and the PS2 running, Esc releases the keyboard, focuses `#capture`, and announces "Keyboard released."

Delete `Escape` from `KEY_NAMES`. In fullscreen the browser uses Esc to exit; the page doesn't use Keyboard Lock to stop it. If fullscreen ends while running and the page didn't end it (`!page.fsByPage`), release the keyboard to `#capture` and show the controls.

**Page shortcuts** run at the top of `onKeyDown`, before the `if (!name) return` gate. Each one calls `preventDefault()` and adds its `e.code` to `swallowed`, so the matching keyup is never forwarded to the PS2.

| Key | Action | Guard |
|---|---|---|
| Alt+Enter | Fullscreen | Works anywhere. Alt combos are never forwarded. |
| Enter | Boot | Screen focused, and the state is ready, stopped or failed. |
| `?` (matched on `e.key`) | Keys drawer | Focus is in the theatre. Skip it if `e.code` is a pad key. On AZERTY, `?` is Shift+KeyM, and that keypress is swallowed. |
| `` ` `` (Backquote; ² on AZERTY) | Log drawer | Focus is in the theatre, with no modifiers. |
| Double-click the picture | Fullscreen | Only while running. |

The single-key shortcuts only work while focus is in the theatre (WCAG 2.1.4). There is no pause key, because pause doesn't exist yet (P stays free). There is no Stop key, because there are no save states and a stray press would lose the session.

**Hiding the controls.** Set `data-chrome="hidden"` only when all of these hold:
- the PS2 is running, the keyboard is captured, and the window has focus
- no drawer, popover, dialog or subtitle is open
- the pointer is not over the controls
- "Keep controls visible" is off, and the device has hover
- there has been no pointer move over 4 px, pointer-down or wheel for 2.5 s

Pointer activity, focus entering the controls (they show at once), a page shortcut, Esc or any state change brings them back. Pad keys never do. The pointermove handler only resets one timer and writes the attribute when its value changes; it never calls `render()`. While the controls are hidden, the cursor is `none` over the screen.

**Fullscreen.** The sill and transport become a flat `rgb(0 0 0 / .86)` band over the bottom of the picture, with an open drawer above the band. The band hides the same way. The speed and the tally stay visible in a black box at the bottom right.

## 9. Implementation notes

```html
<body data-state="unsupported|dead|nobios|ready|queued|starting|running|stopped|failed"
      data-runtime="deferred|loading|init|ready" data-lights="up|down"
      data-chrome="shown|hidden" data-capture="on|off|na" data-drag="ok|blocked|">
 header.marquee > h1.wordmark(svg.mark, "PCSX2 <span>Web</span>"), button#about-btn
 main#theatre                                    fullscreen target
   div.stage-box                                 container-type: size
     div#screen[tabindex=0][role=application] > canvas#canvas, div.hairline[role=progressbar]
     section#slate > h2[tabindex=-1], p.body, pre.excerpt, div.actions, p.error, details.dev, p.fine
   div.sill > div.capture, p#subtitle[role=status], p#readout, span.tally
   div.transport[role=group] > .t-left(#btn-stop #disc-btn #bios-btn) .t-right(#btn-crisp #btn-keys #btn-log #btn-fs)
   section#keys-drawer[hidden], section#log-drawer[hidden] > div#log[role=log], div#disc-menu[popover], div#bios-menu[popover]
 footer.credits, dialog#about, p#announce[role=status], input#bios-input[type=file][hidden], input#disc-input[type=file][hidden]
```

**CSS.** One file of about 650 lines, in this order: `@font-face`, tokens, base, layout, components, states, media queries.
- `.theatre` is a grid with rows `minmax(0,1fr) auto auto auto`.
- `.stage-box { container-type: size; display: grid; place-items: center; padding: 4px }`.
- `.screen` and the desktop `.slate` share `grid-area: 1/1`, with `width: min(100cqw, calc(100cqh * 4 / 3)); aspect-ratio: 4/3`. This replaces `--stage-reserve`.
- A `ResizeObserver` sets `--screen-w`, which sizes the sill and transport.
- Below 640 px the slate drops into normal flow under the screen.
- States are styled only through `body[data-*]` selectors.
- In `forced-colors` mode:
  - frame line and tally: `Highlight` when captured, `CanvasText` when released
  - primary button: a `ButtonText` border
  - hairline: `Highlight`
  - pressed toggles: a `Highlight` underline
- Icons use `<use href="icons.svg#play"/>`.

**JS.** Leave the engine alone: Module setup, `instantiateWasm` progress, `drawLoop`, the key queue, IndexedDB, polling and `window.PCSX2Web`.
- **Keys:** delete `Escape` from `KEY_NAMES`, and add `PAD_CODES`, `onShortcut()` and `swallowed`.
- **Renames:**
  - `renderStatus` becomes `renderReadout`.
  - `renderPlaceholder` becomes `renderSlate`, fed by one `COPY` table.
  - `renderBiosCard` becomes `renderBiosMenu`.
  - `renderKeyboardChip` becomes `renderCapture`.
  - `toast` becomes `subtitle`.
- **Replaced:** `showBanner` gives way to slate variants and inline errors. `renderProgress` now drives the hairline and the sill.
- **Changed behaviour:**
  - `environmentProblem()` returns `{ cause, checks: [{ label, pass }] }`.
  - `boot()` while the runtime is loading toggles `page.autoBoot`.
  - Add `page.runStartedAt`, and set `page.bios.saveFailed` when `idbPut` fails.
- **New pieces:**
  - the controls-hiding logic and `data-lights`
  - the drawers (`logIsOpen()` reads the Log drawer)
  - popover placement on `beforetoggle`
  - keyboard-layout labels
  - fullscreen on `#theatre`
  - a deferred `loadRuntime()` on touch-only devices
- **Source link:** every Source link points to `SOURCE_URL = "https://github.com/TabStationToo/TabStationToo"`, the public repository (its default branch is `wasm`).

**Delete:**
- the top bar: `.topbar`, `.brand` and its logo, the `.tag` chip, the `.status` pill and its dot
- the side column: `#kb-chip`, `.side` and every `.card`
- the old messages: `#banners`, the gradient `.placeholder`, `.spinner`, `.progress`
- the `<details>` console, `.drop-overlay`, `.toast`, `.hint`, `.legal` and the footer paragraph
- the inline symbols and the data-URI favicon
- the old tokens: `--ps-*`, the blue `--accent`, the `system-ui` stacks, `--stage-reserve`

In the `index.html` comment, the `style.css` header and the README's Serve section, change "no web fonts" to "same-origin, self-hosted fonts". Update the README's Use section to match the new controls.

## 10. Disc

A disc is the second thing the page asks for, after the BIOS. It is optional: Boot with no disc still opens the console's own menus. Nothing is copied or uploaded; the disc worker reads the user's file in place (`docs/web-port/DISC-DESIGN.md`).

**Picking.**
- The BIOS title card gets a quiet "Choose a disc…" between Boot and "Use another BIOS…". It opens the File System Access picker where the browser has one (so the disc can be reopened next visit), otherwise `input#disc-input[type=file]`. Accepted: `.iso`, `.bin`, `.img`.
- Drag and drop anywhere, as for the BIOS. A dropped file is a disc when it holds an ISO 9660 volume at one of the layouts PCSX2 detects (2048, 2336, 2352 or 2448-byte sectors, with or without a Nero header); otherwise the BIOS rules apply. With a BIOS chosen the drop slate reads "Drop to use this disc or BIOS."
- `?disc=<url>` (same origin only, like `?bios=`) opens a URL served with byte ranges, for development and tests. With `?bios=` too, the boot is queued as usual; if the disc fails, its error stays on the BIOS title card and the queued boot does not run. A server that has not answered within 15 s counts as failed ("(no answer in time)"). The value is a URL inside a query, so it is percent-encoded once more (`serve.py --disc` prints ready links).
- A disc chosen before any BIOS: the intake's fine print adds "<disc file> is chosen. Add a BIOS to boot it." Nothing boots by itself once the BIOS is in.
- **Reopening after a reload** (Chromium): the file handle is kept in IndexedDB. The BIOS title card shows the fine-print line "Last disc: <name> · Open it again". "Open it again" is a link-style button; one press asks the browser for permission, then opens the disc. Nothing reopens or boots by itself.
- While the PS2 is starting or running the disc cannot change: picks and drops are refused with "Stop the PS2 first to change the disc." There is no swap or eject while running (PCSX2 can reopen the old disc only if it stays readable, and one worker serves one file).

**Shown.**
- The **disc title card** replaces the BIOS title card: the game's title (from PCSX2's game database once a boot has reported it, else the file name without its extension and region tags), then `<file> · 3.2 GB · <where>`, where is "Read from your file", "Remembered in this browser" (handle kept) or "Loaded from ?disc=, not saved". Actions: "Boot" (Enter), "Use another disc…", "Remove the disc". Fine print: "With <BIOS title>. Saves go to a memory card kept in this browser." (or "Saves last for this visit only." when IndexedDB is unavailable).
- **Boot** first reads the start of the disc (milliseconds; at most 5 s for a server that does not answer). If that fails, nothing boots and the slate shows the error below, so a disc that went away never leaves the boot hanging in "Starting the PS2…". Meanwhile the boot is queued: past 0.3 s the slate is the queued variant titled "Reading the disc…" (announced "Reading the disc."), and pressing Boot again cancels it. Boot pressed while the disc is still opening (`?disc=`, a disc just chosen, a disc reader being restarted) is queued the same way.
- **Starting:** "Starting the PS2…" / `<disc file> · 4 s`. **Stopped:** `<game title> · ran 04:12`. **Failed:** "PCSX2 couldn't boot <disc file>. The end of the log:".
- **Transport:** a disc menu button (`#disc-btn`, icon `disc`, the game's title, ellipsised at 16rem; 9rem under 640 px) sits left of the BIOS menu and appears only while a disc is chosen. Its popover (`#disc-menu`, like the BIOS menu) shows the file name and where it comes from, then "Choose another disc…" and "Remove the disc". While the PS2 runs, the items are `aria-disabled` and described by "Stop the PS2 to change the disc." The button's accessible name is "Disc: <title>".
- While a game runs the document title is `<game title> · TabStationToo`.

**Removed.** "Remove the disc" (title card or disc menu) forgets the disc and its remembered handle; the BIOS title card comes back and focus goes to Boot. The subtitle confirms: "Disc removed. Boot opens the console's menus."

**Errors** (the slate's coral line; while running, the slate is hidden, so a disc failure shows as a subtitle that stays 8 s, and again on the slate after Stop):
- "That file isn't a disc image TabStationToo can read. Use an .iso, .bin or .img of a PS2 disc."
- "Compressed images (.chd, .cso, .zso, .gz) and block dumps aren't supported yet. Use an .iso."
- "Couldn't read the disc file. It may have been moved, changed or deleted; choose it again."
- "The browser didn't allow reading <name>. Choose it again."
- "Couldn't open the disc at /dev-disc/x.iso (HTTP 404)." (or "(network error)", "(no answer in time)", "(no byte ranges)", "(the disc changed)") · "?disc= only accepts addresses on this server." These are also the slate's line after Stop when a `?disc=` disc failed while running.
- While running, from a file: "The disc can't be read any more. Stop, then choose the file again." From `?disc=`: "The disc server stopped answering, so the game can't read its disc." (no answer, or no connection) or "The game can't read its disc from the server any more." (anything else). A disc reader that had to be restarted and could not take the disc over counts as a read failure. A reader that stops while it opens a disc is restarted and opens that disc again; when no reader can start any more: "The disc reader stopped working. Reload the page to read discs again."
- When reads work again during the same run (the server answers again): the subtitle "The disc can be read again. If the game doesn't go on, stop it and boot again." (a game that already got a read error may not retry).

**Memory card.** Slot 1 is a memory card file kept in IndexedDB: copied there 1.5 s after the game stops writing to it, at once when the PS2 stops (only if the card changed), and when the page is hidden. From the first write until that copy is done, closing or reloading the page brings up the browser's own "Leave site?" question, because the copy cannot finish during an unload. There is no memory-card control yet. A copy that fails is tried again after 2, 5 and 15 s (the copy when the page is hidden or left does not wait for that pause, and its failure does not count). After that the page stops trying until the game writes to the card again and drops the "Leave site?" question; the fine print reads "Saves last for this visit only." until a copy works again, and one subtitle says "Couldn't keep the memory card in this browser. Saves last for this visit."
- One tab keeps the card: the first TabStationToo tab open in the browser. A second tab loads the saved card but never writes it back, so it cannot overwrite the first tab's saves; its fine print reads "Saves last for this visit only: TabStationToo is open in another tab." Reloading it once the first tab is closed makes it the one that keeps the card.

**Announcer:** "Disc ready." · "Disc removed." · "Reading the disc."
