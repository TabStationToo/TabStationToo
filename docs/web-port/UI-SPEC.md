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

There is also a BIOS-menu popover, a Sound popover (§11), Graphics and Underclock popovers (§12, §12a) and an About dialog. Before play, the slate holds the single primary action. During play, the transport does.

Space reserved for later, with no controls today:
- save-state slots, left of the memory cards (the disc slot and the memory-card slots are in use, see §10 and §14)
- Settings, as a third drawer

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
- Set: play, stop, memory, caret-up, folder-open, trash, checkerboard, keyboard, terminal-window, corners-out, corners-in, copy, x, arrow-clockwise, arrow-up-right, arrow-line-down, arrow-up/down/left/right, disc, and speaker-high/low/x/slash (§11).
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
  - Left: Stop (while starting or running), then Card 1 and Card 2 (§14), then the disc menu (while a disc is chosen, §10), then the BIOS menu.
  - Right: Sound (a popover, §11), Graphics (a popover, §12), Underclock (a popover, §12a), Crisp (`aria-pressed`), Keys and Log (`aria-expanded`), Fullscreen.
  - Labels show at 1024 px and wider. Narrower than that, buttons are icons only, with a CSS tooltip on hover and `:focus-visible` that includes the shortcut. On `hover: none` devices the labels sit under the icons.
- **BIOS menu** (popover).
  - The trigger has `aria-expanded` and `aria-controls` and is never disabled. The menu opens upward and focuses its first item.
  - It shows the file name and where it is stored, then "Choose another file…" and "Forget this BIOS".
  - While the PS2 is running, the items are `aria-disabled` and described by the line that gives the reason.
  - Forget asks for confirmation inline, with focus on Keep.
- **Keys drawer.** Two tabs, Keyboard and Gamepad (§13).
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
  - Booting a `.gz` disc, which PCSX2 reads whole to index it first: at 10 s "Still starting. A .gz image is read whole before the game starts: about a minute for a DVD.", and the 15 s line only at 180 s.
- **Boot refused:** "The emulator is already starting." / "The emulator refused to boot. The log has details."
- **Capture:** "Keys go to the PS2" / "Click the screen to play" / when the button has focus: "Press Enter to play"
- **Hints:**
  - First boot: "Arrows move · K confirms (L on Japanese consoles) · ? shows every key"
  - Once per visit, after 5 s under 50% speed: "Running at 18%. Heavy scenes are slow in the browser for now."
  - When the GPU fails during play and the picture moves to the 2D canvas (not when WebGPU was never available): "The GPU stopped working, so the picture is drawn without it now."
- **Keys drawer:**
  - "In the system menus ✕ confirms and △ goes back (○ confirms on Japanese consoles)." The glyphs are SVG.
  - "While the PS2 runs, keys you press light up here."
  - "Keep controls visible"
  - Tabs: "Keyboard" · "Gamepad". Keyboard player: "The keyboard plays as" with "Player 1" · "Player 2".
  - Mapping: "Map controls" · "Map again" · "Clear mapping" · "Skip" · "Back" · "Cancel" · "Step 3 of 21 · Last: Circle: button 1" · "This gamepad does not report a standard layout, so the page cannot tell which control is which. Map it once: press each control when asked. It is remembered for this gamepad in this browser." · "You mapped this gamepad. Press its controls to check that the right ones light up, or click one in the picture to map it again." · announcer: "<name> is mapped. Press its controls to check them." · "Mapping cancelled." · "The mapping of <name> is cleared."
  - Gamepad tab: "Press a button on your gamepad to connect it." (no gamepad), "Swap players", "Not used: two gamepads at most."
  - Announcer: "Gamepad connected: <name>, Player 1." · "Gamepad connected: <name>. Only two gamepads play, so it is not used." · "Gamepad disconnected: <name>." · "<name> plays as Player 2." · "The keyboard plays as Player 2." · "Players swapped. <name> is Player 2. <name> is Player 1."
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
  - "Works today: the BIOS menus and games from disc images, with sound, a keyboard and gamepads, and memory cards kept in this browser or in files on your disk. Many games run at or near full speed, the heaviest scenes still below. Save states are kept in this browser."
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
| M (matched on `e.key`, so the key labelled M on any layout) | Mute or unmute (§11) | Focus is in the theatre or in the Sound menu, with no modifiers. Skip it if `e.code` is a pad key. |
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
   div.transport[role=group] > .t-left(#btn-stop #disc-btn #bios-btn) .t-right(#btn-sound #btn-crisp #btn-keys #btn-log #btn-fs)
   section#keys-drawer[hidden] > div.tabs[role=tablist], div#panel-keyboard[role=tabpanel], div#panel-gamepad[role=tabpanel], section#log-drawer[hidden] > div#log[role=log], div#disc-menu[popover], div#bios-menu[popover], div#sound-menu[popover]
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
- The **disc title card** replaces the BIOS title card: the game's title (from PCSX2's game database once a boot has reported it, else the file name without its extension and region tags), then `<file> · 3.2 GB · <where>`, where is "Read from your file", "Remembered in this browser" (handle kept) or "Loaded from ?disc=, not saved". Actions: "Boot" (Enter), "Use another disc…", "Remove the disc". Fine print: "With <BIOS title>." and the memory-card line of §14 (or "Saves last for this visit only." when IndexedDB is unavailable).
- **Boot** first reads the start of the disc (milliseconds; at most 5 s for a server that does not answer). If that fails, nothing boots and the slate shows the error below, so a disc that went away never leaves the boot hanging in "Starting the PS2…". Meanwhile the boot is queued: past 0.3 s the slate is the queued variant titled "Reading the disc…" (announced "Reading the disc."), and pressing Boot again cancels it. Boot pressed while the disc is still opening (`?disc=`, a disc just chosen, a disc reader being restarted) is queued the same way.
- **Starting:** "Starting the PS2…" / `<disc file> · 4 s`. **Stopped:** `<game title> · ran 04:12`. **Failed:** "PCSX2 couldn't boot <disc file>. The end of the log:".
- **Transport:** a disc menu button (`#disc-btn`, icon `disc`, the game's title, ellipsised at 16rem; 9rem under 640 px) sits left of the BIOS menu and appears only while a disc is chosen. Its popover (`#disc-menu`, like the BIOS menu) shows the file name and where it comes from, then "Choose another disc…" and "Remove the disc". While the PS2 runs, the items change the disc without stopping it (Swap while running, below). The button's accessible name is "Disc: <title>".
- While a game runs the document title is `<game title> · TabStationToo`.

**Swap while running.** While a game (or the console's menus) runs, "Choose another disc…", "Open it again" and a disc dropped on the page change the disc without stopping it, as on the console with games on several discs: the tray opens (PCSX2 ejects the disc), the new file opens, the disc goes in. The subtitle says "Changing the disc…", then "Disc changed: <file>.", and the game's title follows when PCSX2 reports it. "Remove the disc" ejects: "Disc removed. The game goes on with the tray empty." Failures (8 s subtitles): "Couldn't change the disc. The game goes on with the one it had." · "Couldn't open that disc, so the old one went back in." · "Couldn't open that disc. The tray is empty; choose a disc again." · "PCSX2 couldn't read that disc. The tray is empty; choose another." "Disc library…" stays `aria-disabled` while a game runs (it boots), described by "Stop the PS2 to use the disc library. Choose another disc… changes it while it runs."; while the PS2 starts or a swap is under way, the other items are too, described by "The disc can change once the PS2 runs."

**Removed.** "Remove the disc" (title card or disc menu) forgets the disc and its remembered handle; the BIOS title card comes back and focus goes to Boot. The subtitle confirms: "Disc removed. Boot opens the console's menus."

**Errors** (the slate's coral line; while running, the slate is hidden, so a disc failure shows as a subtitle that stays 8 s, and again on the slate after Stop):
- "That file isn't a disc image TabStationToo can read. Use an .iso, .bin, .img, .chd or .cso of a PS2 disc." (also a `.chd`, `.cso`, `.zso` or `.gz` whose first bytes are not that format's header)
- "Block dumps (.dump) aren't supported. Use an .iso, .chd or .cso."
- "Couldn't read the disc file. It may have been moved, changed or deleted; choose it again."
- "The browser didn't allow reading <name>. Choose it again."
- "Couldn't open the disc at /dev-disc/x.iso (HTTP 404)." (or "(network error)", "(no answer in time)", "(no byte ranges)", "(the disc changed)") · "?disc= only accepts addresses on this server." These are also the slate's line after Stop when a `?disc=` disc failed while running.
- While running, from a file: "The disc can't be read any more. Stop, then choose the file again." From `?disc=`: "The disc server stopped answering, so the game can't read its disc." (no answer, or no connection) or "The game can't read its disc from the server any more." (anything else). A disc reader that had to be restarted and could not take the disc over counts as a read failure. A reader that stops while it opens a disc is restarted and opens that disc again; when no reader can start any more: "The disc reader stopped working. Reload the page to read discs again."
- When reads work again during the same run (the server answers again): the subtitle "The disc can be read again. If the game doesn't go on, stop it and boot again." (a game that already got a read error may not retry).

**Memory cards:** §14.

**Announcer:** "Disc ready." · "Disc removed." · "Reading the disc."

## 11. Audio

The PS2's sound plays in the page (`docs/web-port/AUDIO-DESIGN.md`). There is one control, **Sound**, in the slot §2 kept for volume: first on the right of the transport, before Crisp. Volume and mute are the page's own; they never change what the PS2 mixes.

**Sound button** (`#btn-sound`, `.t-btn`, `popovertarget="sound-menu"`, `aria-controls`, `aria-expanded`). It is never disabled.
- Icon, Phosphor Regular: `speaker-high` at 50% or more, `speaker-low` below 50%, `speaker-x` when muted or at 0%, `speaker-slash` while the browser holds the sound or when the browser can't play it.
- Label "Sound". Its accessible name (`aria-label`, which starts with the visible label) adds the state: "Sound, 80 percent", "Sound, muted", "Sound, waiting for a key or click", "Sound, unavailable". It carries `aria-keyshortcuts="M"`.
- Tooltip (icons-only widths): "Sound" with the key cap M.
- The pressed bar shows while the menu is open, like Keys and Log.

**Sound menu** (`#sound-menu`, popover, `role="group"`, `aria-label="Sound"`), styled like the BIOS menu. It opens upward, right-aligned to its trigger, and focuses its first control. It closes on Esc, a click outside, or when Tab takes focus out of it, and focus goes back to the button.
- **Volume:** `input[type=range]`, 0 to 100 in steps of 5, labelled "Volume". The value sits to its right in Chivo Mono ("80%") and is `aria-valuetext` "80 percent". Arrows step 5, Page Up/Down 10, Home/End go to the ends. The track is a 2 px `--line-2` line, filled in `--ink` up to the thumb; the thumb is a 12 px `--ink` square with a 3 px radius (amber stays for input capture). Hit area 40 px tall, 44 px on coarse pointers.
- **Mute:** a menu item with `aria-pressed`, icon `speaker-x`, "Mute", and an 11 px mono key cap "M" (hidden on touch-only devices). Pressed, it gets the 2 px ink bar under its icon, as pressed toggles do in the transport.
- **Fine print**, only when it applies, in `.menu-reason` style: "The browser holds the sound until you press a key or click." or "This browser can't play the emulator's sound."
- Moving the slider while muted unmutes. Changes apply at once and are saved on `change`.

**Shortcut M** mutes or unmutes (§8). The subtitle says "Sound off." or "Sound on." (it is the live region; nothing else is announced).

**Starting sound.** Browsers start sound only after a key press or click on the page.
- Boot (the button, Enter on the screen, or a queued boot's press) makes the AudioContext inside that gesture, so sound starts with the first frames.
- `?bios=` boots without a gesture. The PS2 runs silently until the first key press or click anywhere on the page, which turns the sound on and still does what it was meant to do (a key still reaches the PS2). With the first frame, the subtitle says "Press a key or click to turn the sound on." (instead of the first-boot hint, which then waits for the next boot), and the Sound button shows `speaker-slash`.
- If the browser interrupts or suspends the sound later, the button and the menu show the same state, one attempt to resume is made at once, and the next key press or click resumes it.
- If the browser has no Web Audio worklets, or the worklet fails to start, the button shows `speaker-slash` ("Sound, unavailable"), the menu's fine print says so, and with the first frame the subtitle says "This browser can't play the emulator's sound." once per visit. The PS2 runs on.

**Running, stopping, hidden tab.**
- Stop (and failed or fatal) fades the sound out over 30 ms and then suspends it, so a stopped page costs no audio work. Boot again resumes it.
- A hidden tab keeps playing. The browser shows its own speaker on the tab, and the tab can be muted there.
- Slow scenes play stretched: continuous, at the same pitch, and slowed down with the picture.

**Remembered per browser** (`localStorage`, failing quietly like `pcsx2web.crisp`): `pcsx2web.volume` (`"0"` to `"100"`, default 100) and `pcsx2web.muted` (`"1"` or `"0"`, default `"0"`). The gain is (volume / 100)², ramped over about 15 ms so changes never click.

**Test switches** (no controls): `?mute=1` forces the output gain to 0 for the visit without touching the stored values; `?audio=null` runs the PS2 with no sound at all (PCSX2's Null backend), for cost comparisons; `?audiotap=SECONDS` records what the worklet plays, before the gain, for `window.PCSX2WebAudio` (at most that many seconds); `?audiorate=HZ` makes the AudioContext at another rate than 48 kHz, so the emulator's resampling (PS1 mode) can be tested.

**Motion and accessibility.** Nothing animates beyond the popover rules. The slider and Mute work by keyboard, with visible focus. In forced-colors mode the slider uses `ButtonText` for the track and thumb and `Highlight` for the filled part and focus.

**Microcopy:** "Sound" · "Volume" · "Mute" · "80%" · "Sound off." · "Sound on." · "Press a key or click to turn the sound on." · "The browser holds the sound until you press a key or click." · "This browser can't play the emulator's sound." · accessible names "Sound, 80 percent", "Sound, muted", "Sound, waiting for a key or click", "Sound, unavailable".

**Phone (under 640 px).** Sound leads the tools row: Sound, Crisp, Keys, Log, Fullscreen. The menu keeps its 296 px (it fits in 358 px) and stays right-aligned to the button inside the gutters.

## 12. Graphics

The picture comes one of three ways, and the page says which. **GPU renderer**: PCSX2's hardware renderer on WebGPU (the default). **GPU display, software drawing**: the software renderer draws and WebGPU shows the frames (`?renderer=sw`, or an adapter without `dual-source-blending`, the hardware renderer's one hard requirement). **Software on a 2D canvas**: no WebGPU at all (`?gpu=0`, no `navigator.gpu`, no JSPI, no adapter, or no device). The GS reports what it opened and why (`WebGPUHost::NoteRenderer`, a `mode` message on the frame port or the `tstGpuMode` handler); the page log gets one line, "Graphics: Software on a 2D canvas (the browser gave no WebGPU adapter …)."

**Graphics button** (`#btn-gfx`, `.t-btn`, `popovertarget="gfx-menu"`), right after Sound. Icon Phosphor `graphics-card`, label "Graphics"; its accessible name adds the mode once the GS has opened: "Graphics, GPU renderer". Never disabled (the check is useful on an unsupported page too).

**Graphics menu** (`#gfx-menu`, popover, `role="group"`, 320 px, styled and placed like the Sound menu: upward, right-aligned, Esc / click outside / Tab out close it, focus returns to the button).
- **Now:** the mode in 15 px ("Not started yet" before the first boot) and, when WebGPU or its renderer isn't used, "Because …" with the reason in plain words.
- **Renderer:** a fieldset of three radios, focused on open (arrows move between them): **Auto** "GPU when it works, software otherwise" · **GPU** "PCSX2's hardware renderer on WebGPU" (`?renderer=hw`) · **Software** "No WebGPU: drawn by the CPU on a 2D canvas" (`?gpu=0`). Remembered per browser in `pcsx2web.renderer` (`auto`, `gpu`, `sw`; quiet failure like the other preferences). The runtime reads it when it starts, so the fine print says "Remembered in this browser. A change applies after a reload.", "Applies when the page is reloaded." after a change, or "The address's ?gpu= or ?renderer= decides for this visit." when either switch is in the address (the address wins).
- **Resolution:** a second fieldset of two radios, after Renderer: **Native** "As the PS2 draws it" (the GS's upscale multiplier 1, the default) · **2×** "Twice the lines; needs a stronger GPU" (multiplier 2). It applies at once, mid-game (`pcsx2web_set_upscale()`, at the next vsync or boot), and is remembered per browser in `pcsx2web.upscale` (`"1"` or `"2"`, quiet failure like the other preferences). Only the GPU renderer draws above native, so when the GS opened in software the fine print says "Only the GPU renderer draws at 2×; this picture is drawn in software. Remembered in this browser."; otherwise "Remembered in this browser. Applies at once." The GS clamps the multiplier to what the adapter's largest texture allows. The picture is still presented at the canvas's size, so on a large display 2× shows a sharper frame, not a bigger one.
- **Picture:** a third fieldset of four radios, after Resolution: **Smooth** "Soft edges" · **Crisp** "Hard pixels" · **CRT** "Curved tube, scanlines" · **CRT 2** "The same with glow and trails" (the picture modes of §16's PICTURE button, which steps through the same four; either one moves the other). It applies at once and is remembered per browser in `pcsx2web.picture`; the subtitle names the choice ("Picture: CRT 2."). CRT and CRT 2 are shaders of the WebGPU device: on the 2D canvas their radios are disabled and the fine print says "CRT needs WebGPU graphics; this picture is on a 2D canvas. Remembered in this browser."; otherwise "Remembered in this browser. Applies at once."
- **Check WebGPU:** a menu item. It tests `navigator.gpu`, asks for an adapter (high-performance, then default) and a device with the renderer's features in the page itself, and reports in a polite live region, then in the log: "WebGPU works on amd rdna-3. The GPU renderer can run." · "WebGPU works on …, but it lacks dual-source-blending, which the GPU renderer needs: the picture is drawn in software and shown through WebGPU." · "WebGPU is there, but the browser gave no adapter: …" · "No WebGPU: this browser doesn't offer it here." · "The browser gave an adapter (…) but refused a device: …". Missing optional features follow by name ("Missing optional features: texture-compression-bc."). On Linux desktop Chromium browsers, when there is no adapter or no WebGPU, it adds: "On Linux this browser keeps WebGPU off for most GPUs. Open chrome://flags (brave://flags, edge://flags, opera://flags as fits), enable "Unsafe WebGPU Support" and "Vulkan", then relaunch the browser."

**Why an unflagged Linux Chromium gets the 2D canvas.** It exposes `navigator.gpu` but answers `requestAdapter()` with null on GPUs outside its allowlist (measured on Brave 155 with a Radeon 780M on Mesa RADV: null without `--enable-unsafe-webgpu`, a full-featured adapter with it). The GS retries six times over about 3 s, alternating power preferences, then draws through the 2D canvas and says so.

**Phone (under 640 px).** The tools row is Sound, Graphics, Underclock, Crisp, Keys, Log, Fullscreen; on touch each tool is at least 48 px wide, with no gap, so the seven fit in 358 px. The menu is at most the width between the gutters and scrolls if taller than the screen.

**Firefox.** Auto tries WebGPU in Firefox (Zen and other Gecko browsers) as in the others. Firefox drops the GS canvas's WebGPU configuration each time it hands a frame over, so the GS configures it again after every frame (`docs/web-port/WEBGPU-DESIGN.md` §12.1); without that only the first frame showed.

**Microcopy:** "Graphics" · "Now" · "Not started yet" · "GPU renderer" · "GPU display, software drawing" · "Software on a 2D canvas" · "Because …" · "Renderer" · "Auto" · "GPU" · "Software" · "Resolution" · "Native" · "2×" · "Picture" · "Smooth" · "Crisp" · "CRT" · "CRT 2" · "Check WebGPU" · "Checking…".

## 12a. Underclock

PCSX2's EE Cycle Rate speedhack, chosen per game: the PS2's main processor (the EE) runs at 75, 60 or 50 % of its speed, so a heavy scene needs fewer instructions per frame and runs faster in the browser. Some games then run their own logic in slow motion or glitch, so it is off unless chosen, and only for the game it was chosen for.

**Underclock button** (`#btn-ee`, `.t-btn`, `popovertarget="ee-menu"`), right after Graphics. Icon Phosphor `gauge`, label "Underclock" ("Clock" under the icon on touch phones, where the seven tools share 358 px); its accessible name adds the choice ("Underclock, off", "Underclock, medium") and its tooltip names a choice that is on ("Underclock: medium"). While a choice other than Off is on, it carries the pressed toggles' 2 px ink bar (`.on`). Never disabled.

**Underclock menu** (`#ee-menu`, popover, `role="group"`, 320 px, styled and placed like the Graphics menu).
- **This game:** the running game's title (or its serial), "Not a game with a serial", or "No game running".
- **Underclock the PS2's processor:** a fieldset of four radios, focused on open: **Off** "Full speed, as on a PS2" · **Light** "75 %" · **Medium** "60 %" · **Strong** "50 %" (EE cycle rate 0, -1, -2, -3). They are disabled until the running game's serial is known. A choice applies at once (`pcsx2web_set_ee_cycle_rate()`, at the next vsync) and is remembered per serial in `pcsx2web.eeRate.<serial>`; every boot starts at Off, and the game's choice applies when its serial is known.
- Fine print (the fieldset's description): "Remembered for this game. Some games run in slow motion or glitch underclocked." or, without a game, "Start a game to choose. Each game keeps its own choice."

**Microcopy:** "Underclock" · "Clock" · "This game" · "No game running" · "Not a game with a serial" · "Underclock the PS2's processor" · "Off" · "Light" · "Medium" · "Strong" · "Full speed, as on a PS2".

## 13. Players and gamepads

The PS2 has two pad ports. Each device the page can play with (the keyboard, and each gamepad) plays as Player 1 (port 1) or Player 2 (port 2). Any device can take either port, and two devices can share one: the port plays the larger of their values, so a key and a stick never cancel each other.

**Gamepads.** A gamepad that the browser reports with the Gamepad API's `"standard"` layout (Xbox, DualShock, DualSense, Switch Pro and most others) plays at once. Any other gamepad is listed too, and its owner maps it (below). The standard layout goes by position: the bottom face button is Cross, right Circle, left Square, top Triangle. L2 and R2 pass their pressure; the sticks are analog; the Guide/Home button is the PS2's Analog toggle (some browsers and systems keep it for themselves). A round dead zone of 10 % is applied by the page (rescaled, so the full throw still reaches 1), and the core's diagonal scale correction is on at 100 % sensitivity, so a round gate reaches the corners. Standard gamepads have a fixed mapping: no remapping, rumble or menu navigation by gamepad yet.

**A gamepad the browser does not lay out in the standard way** (an 8BitDo in D mode on a Mac, for one) is mapped by its owner, in its section of the Gamepad tab. A short note says so, with **Map controls**. The run asks for one control at a time, in this order: Cross, Circle, Square, Triangle, L1, R1, L2, R2, Select, Start, D-pad Up, Right, Down, Left, L3, R3, left stick right and up, right stick right and up, Analog. The control asked for gets a dashed amber outline in the picture, and a polite status line gives the prompt ("Press the bottom face button (Cross).") above "Step 3 of 21 · Last: Circle: button 1". **Skip**, **Back** and **Cancel** are buttons; each prompt listens at once, so the owner can map at their own pace: a press counts when it is let go (or after 0.3 s held, collecting what moves meanwhile), and the next prompt starts once it is let go and its axes are back near where they started (at most 2.5 s). A control held when a prompt appears counts only once let go and pressed again. (A 0.35 s quiet time before each prompt once dropped presses made at a normal pace and gave them to the next prompt.) A button, or an axis for a trigger or a D-pad, is accepted; a trigger on an axis keeps its pressure (its rest value and pressed value are recorded); a D-pad on one axis (a hat switch) maps all four directions at once. Afterwards the note says "You mapped this gamepad", the picture shows what it presses live, a click on a control in the picture maps just that one (a D-pad button the whole D-pad, a stick both its axes), **Map again** repeats the run and **Clear mapping** forgets it. Until it is mapped the gamepad plays nothing, and the announcer says so ("Its layout is not standard: open Keys, then Gamepad, to map its controls."). The mapping is kept per gamepad model, button count and axis count (`pcsx2web.padMaps` in `localStorage`), because the browsers number a gamepad's buttons and axes differently. An axis that has not yet shown its rest value (some browsers report 0 until it first moves) is ignored.

**Which port.** The first two gamepads that connect take Player 1 and Player 2 (a first gamepad starts on the port saved as `pcsx2web.padFirst`, the second on the other one); a third and later one is not used. The keyboard starts on the port saved as `pcsx2web.padKeyboard` (default Player 1). Defaults: keyboard Player 1, first gamepad Player 1, second gamepad Player 2. A gamepad that was unplugged and plugged in again takes its port back within the visit. Per-device memory is not kept: two identical gamepads share one `id`.

**Looking at a gamepad.** The Gamepad API has no button events, so the page looks at each gamepad's state as often as the browser runs a timer (about every 4 ms, the floor browsers keep for repeating timers), not once per animation frame. Every press and release it sees goes to the PS2 as it is, and the emulator keeps each for at least two vsyncs so the game sees it; a button keeps up to two whole taps waiting, so a burst of taps is not cut short. The page itself never holds a press longer: that merged quick taps into one press when mashing. Only the Gamepad tab's picture shows a press for at least 50 ms, so that a quick tap shows there too. Sticks and triggers follow the gamepad as it moves.

**Quick taps in Firefox.** Firefox (and browsers built on it, such as Zen) can hand the page gamepad changes late and in bursts, so that a quick tap's press and release arrive together and no look ever sees the button down (seen in Zen on macOS with an 8BitDo pad while the emulator runs: about half of quick taps lost; a bare page lost none). Polling cannot recover these: nothing says which button it was. With `dom.gamepad.non_standard_events.enabled` set in `about:config` (on in Nightly, off in release builds), Firefox also fires `gamepadbuttondown`, `gamepadbuttonup` and `gamepadaxismove` for every change, in order. The page plays each event as it comes, on top of the gamepad's state, so every press and release reaches the PS2 even when Firefox hands over a whole tap, or a release and the next press when mashing, at once (with only the presses counted, mashed taps merged into one press: 1 of 10 reached the PS2 in Zen's test gamepad). In Firefox without those events, the Gamepad tab says: "This browser can miss quick taps. To catch every tap, open about:config, set dom.gamepad.non_standard_events.enabled to true, and reload this page."

**When it plays.** A gamepad plays while the PS2 runs, the page has focus and is visible, whatever the state of the keyboard capture (Esc, Tab and a focus on the drawer release only the keyboard). It lets go of everything on blur, a hidden tab, a disconnect, a stop, and when its player changes (what was held then has to be released and pressed again).

**Keys drawer.** Two tabs (`role="tablist"`; Left and Right, Home and End move between them; focus ring as everywhere).
- **Keyboard:** the controller-shaped layout of D4, under "The keyboard plays as" with two radios, "Player 1" and "Player 2".
- **Gamepad:** with none connected, "Press a button on your gamepad to connect it." (a browser lists a gamepad only after its first button press). For each connected gamepad, in connection order: its name (15 px, 560), a "Player 1" / "Player 2" radio pair (the fieldset is named "Player for <name>"; a third gamepad shows "Not used: two gamepads at most." instead), and a live picture of the pad: the same caps as the Keyboard tab (L2, L1, D-pad, Select, Analog, Start, R1, R2, face buttons, L3, R3) that turn amber while held, and each stick as a 44 px circle with a dot that follows it (amber while it is off centre). The picture is `aria-hidden`: it shows state and is not announced. With two gamepads, "Swap players" (`.btn-quiet`) exchanges their ports.
- Changes apply at once and are remembered (`localStorage`, failing quietly).

**Microcopy:** see §7 (Keys drawer and the announcer).

**Tests.** `tools/wasm/pad-test.mjs` plays a mocked `navigator.getGamepads()` and the keyboard in headless Chrome and reads each port's pad through `pcsx2web_test_pad_state`. A real controller is a manual check.

## 14. Memory cards

The PS2 has two memory card slots, Card 1 and Card 2. The page keeps a library of named cards; a slot holds one of them or nothing. A card is an 8 MB `.ps2` file, the format desktop PCSX2 uses, so saves move between the two. Plain words only: "memory card", "Card 1", "Card 2", no card art or logos.

**The library.**
- A card is kept in this browser (IndexedDB), or linked to a `.ps2` file on disk where the browser has the File System Access API (Chromium). A linked card also keeps a copy in the browser; when it is plugged in, the file on disk is read first (the disk wins), and every save is written back to it.
- A card is in one slot at most. Unplugging puts it back in the library; nothing is deleted. Any number of cards (8 MB each); the page asks for persistent storage (`navigator.storage.persist()`).
- A first visit has no cards and both slots empty. A card made by an earlier version of the page (slot 1's `Mcd001.ps2`) becomes "Memory card 1", plugged into Card 1.
- Which card is in which slot, and the last card used in each, are remembered. A linked card whose file the browser has not allowed again since the reload comes back unplugged (one click on it in the picker asks for permission and plugs it in); one the browser still allows stays plugged in.
- Only the cards in the slots are loaded into the emulator.

**Transport.** Two menu buttons sit between Stop and the disc menu, shown while a BIOS is chosen.
- Plugged in: the filled card icon (Phosphor `sim-card`, Fill), ink. A 6 px amber dot on the icon while the game writes to the card (it stays 0.6 s after the last write; colour only, never animated).
- Empty: the outline card icon, ink-3.
- Each shows its slot number and caret; the tooltip (hover, `:focus-visible`) reads "Card 1: RPG saves" or "Card 1 · empty". The transport has no room for names next to the disc and BIOS menus at usual widths, so only a transport 1100 px wide or wider shows "1 · RPG saves" / "Card 1 · empty" (the name ellipsised at 9rem) instead of the tooltip. The slot buttons never shrink; when the left group runs short, the BIOS name gives way first, then the disc title (which keeps at least 4em).
- Accessible name: "Memory card 1: RPG saves, plugged in" or "Memory card 1: empty". `aria-expanded` and `aria-controls` as the other menu buttons.

**Plugged-in menu** (popover, opens upward, focuses its first item):
- The card's name, then where it is: "In this browser", "Linked to `rpg.ps2`", or "Not linked any more" (the file could not be written; the card goes on from the browser's copy).
- **Unplug** (focused), **Rename…**, **Save a copy to disk…** (a save dialog where the browser has one, otherwise a download).
- While the game wrote to the card in the last 5 s (PCSX2's busy time), Unplug waits: the button reads "Card 1 · saving…" and the card unplugs itself once the write is done. The subtitle says "Card 1 will unplug when the game finishes saving."

**Empty-slot menu (the picker).**
- "Card 1 is empty." then the library: the last card used in this slot first, then the most recently used. Each row: the name, and under it "In this browser" or "`rpg.ps2` on disk". A card in the other slot is listed with "In Card 2" and `aria-disabled`. Choosing a card plugs it in.
- Each row has a **More** button (`dots-three`, named "More for RPG saves"). Its view shows the card's name and where it is, then **Rename…**, **Save a copy to disk…**, **Delete** (a browser card) or **Remove from list** (a linked card; the file stays on disk), and **Back to the cards**. Delete is `aria-disabled` for a card in the other slot, described by "Unplug it from Card 2 first."
- Then **New card…** and **Open a card file…**. With no cards, only these two.
- **The first card:** with no card in the library at all, one click on an empty slot makes "Memory card 1" (kept in this browser) and plugs it in, without opening the menu (§16).
- **New card…** asks for a name inline (a line "A new card, for Card 1.", a text field labelled "Name", "Create" and "Cancel"; Rename is the same form with "Rename RPG saves." and "Rename"); the default is "Memory card N" with the lowest free N. The card is kept in this browser (no save dialog: it could not show in an embedded browser or Brave, and creating hung on it); "Save a copy to disk…" makes a file. A new card is blank (all `0xFF`); the game or the console's menus format it.
- **Open a card file…** adds an 8 MB `.ps2` file and plugs it in, named after the file without `.ps2` (" (2)" and so on when the name is taken). A file already linked in the library is plugged in instead of added twice (or refused with "That card is in Card 2."). Anything else is refused: "That file isn't an 8 MB PS2 memory card (.ps2)." A New card saved over a file the library already links is refused: "rpg.ps2 is in the list already, as RPG saves."
- Names: 1 to 32 characters, unique ignoring case. "Give the card a name." / "Another card is called that already."
- **Delete** a browser card asks first: "Delete RPG saves? Its saves are only in this browser." with **Keep** (focused), **Save a copy first…** and **Delete** (coral). Remove from list does not ask. A card in a slot cannot be deleted; it is unplugged first.

**While the PS2 runs.** Cards plug in and out at any time, unlike the disc. PCSX2 reports the slot empty for 60 frames, so the game sees the card change.

**Saving.** A plugged card is copied to IndexedDB 1.5 s after the game stops writing to it, at once when the PS2 stops, when it is unplugged and when the page is hidden; a linked card is then written to its file. From the first write until the copy is done, closing or reloading the page brings up the browser's own "Leave site?" question. A copy that fails is tried again after 2, 5 and 15 s; then the page stops trying until the game writes again, the fine print reads "Saves last for this visit only." and one subtitle says "Couldn't keep the memory card in this browser. Saves last for this visit." A linked file that cannot be written: one subtitle, "Couldn't write rpg.ps2. RPG saves goes on in this browser.", and the menu shows "Not linked any more".

**One tab.** The first TabStationToo tab keeps the cards. A second tab loads the cards in the slots for its visit but never writes them back; its card menus are `aria-disabled` and described by "TabStationToo is open in another tab.", and its fine print reads "Saves last for this visit only: TabStationToo is open in another tab."

**Fine print** on the disc title card, after "With <BIOS title>.":
- No card: "No memory card in. Games won't be able to save. Insert a card" ("Insert a card" opens Card 1's menu).
- "Saves go to RPG saves in Card 1." (or Card 2) / "Saves go to RPG saves in Card 1 and Racing in Card 2."

**Errors** (a coral line in the menu): "The browser has no room for another card. Delete one or save a copy to disk." · "Couldn't read that file. Try choosing it again." · "The browser didn't allow opening rpg.ps2." · "Couldn't save the card here. Try again."

**Subtitles and announcer:** "RPG saves is in Card 1." · "Card 1 unplugged." · "Card 1 will unplug when the game finishes saving." · "RPG saves deleted." · "RPG saves removed from the list. Its file stays on disk." · "Renamed to RPG saves." Errors in a menu are also announced.

**Not here:** multitap slots, PS1 cards, folder cards, cards larger than 8 MB, and a list of the saves on a card.

## 15. Disc library

A second way to choose a disc, from a folder of disc images (`library/disc-library.js`, wired to the page by `library/player-library.js`).

**Opening.** The library is the page's home and the first thing seen: it opens by itself on load, with a BIOS or without, but not when the address asks for a boot (`?bios=`, `?disc=`, `?elf=`) or over another dialog. Until there are discs, the setup shows above the PS2: two tiles side by side (one above the other under 560 px), each with an icon in a rounded square, a name, a line and a button. **BIOS** (a chip icon): "Dumped from your own PS2 (.bin)", "Choose file"; once there is one, a green check, the file's name in mono and a quiet "Change". **Games** (a folder icon): "A folder with your disc images (.iso)", "Choose folder" (a remembered folder: "In “<folder>”; the browser asks once per visit", "Open again" and "Another folder…"), "Reading 3 of 12…" while it reads; dimmed and disabled until there is a BIOS. Errors show in coral under their tile. One line under them: "Choose them or drop them here. They stay in this browser; nothing is uploaded." A file dropped without a BIOS is the BIOS; then a dropped folder or disc images are the games. It opens again by itself when the BIOS goes away (Forget) and when a game stops (Reset, or Eject, which stops a running game first): then it fades in over the player (700 ms) while the PS2 slides back up into it from below (1 s). Close (Esc) leaves the player's slate (Boot opens the console's menus). "Disc library…" sits on the slate before "Choose a disc…" wherever that button shows, and first in the disc menu (`aria-disabled` while the PS2 runs, like its other items). It opens a full-window `dialog` titled "Disc library", with Close (Esc) at the top right and a credit line at the bottom: "Console photo: Evan-Amos, public domain · Disc art: ps2-covers, by serial; your files never leave this browser."

**The folder.** "Choose a folder…" (or a dropped folder). Where the browser has `showDirectoryPicker` (Chromium) the folder is remembered in IndexedDB; on the next visit it is read at once if the browser still allows it, otherwise the intake reads "Your games are in <folder>." with "Open <folder> again" and "Choose another folder…". Each disc image's first sectors give its serial (`SYSTEM.CNF`) and, through `game-titles.tsv` (PCSX2's game database), its title. Discs are sorted by title.

**The scene.** A shelf of discs above a photo of a PS2 (Evan-Amos, public domain). The disc in front is upright and larger, its title, `serial · region · DVD · 4.2 GB` and file name under it. Arrows, Page Up/Down, Home/End, type-ahead, the wheel, drag and fling move along the shelf; Enter, Space or a click on the front disc puts it in; a click on another disc brings it to the front. **A gamepad plays the shelf:** the D-pad (any direction: left and up go back, right and down go forward, as the arrows do) or the left stick (left and right) steps one disc, repeating while held (after 400 ms, then every 110 ms), and Cross (the bottom face button, as everywhere on the page: the A of an Xbox pad) puts the disc in front in; while a disc goes in, Cross counts as Enter for skipping (three quick presses). Any connected gamepad can do it, whichever player it plays as, and a gamepad with its own mapping plays it through that mapping. What a gamepad holds when the library opens is ignored until it is let go (the press that connects a gamepad too), and nothing a gamepad does in the library reaches the PS2: what is still held when the library fades away stays held-back until it is let go. Each disc is printed with an image from the folder named like PCSX2's covers, else the game's cover from the ps2-covers collection (fetched by serial: the one request the page makes to another origin), else a label drawn from its title and serial.

**The insert.** Inside the press, the disc is chosen and its boot queued, as "Choose a disc…" then Boot. The tray (the photo's own front panel) slides out, the disc arcs over and flattens into it, the tray closes, the power light in the Reset button comes on, and the PS2 itself slides down out of the room at once (1.1 s, no text). The library is as dark as the player while a game runs or starts (black), so the fade changes no colour. The dialog then fades out over 700 ms, the player (the screen showing the boot, the console's front, the remote and the controllers) coming in under it, and the screen takes the keyboard. So the boot and the game's black loading frames are never seen. Esc and Close do nothing while a disc goes in. A disc that cannot go in (no BIOS, the PS2 running) says why in coral under the shelf's text.

**The Easter egg.** One insert in ten (never with reduced motion), the disc won't read: the power light blinks, the tray opens again, and the disc comes up to the front, turns over to its data side (dust, smudges, fingerprints, a hair), is sprayed twice and wiped with a cloth in six strokes from the centre out (the right way), catches the light, turns back and goes in again; about 12 s more, before the boot: the player's boot waits until the disc is clean (or the cleaning is skipped), so the game is not heard under it. The announcer says "The disc won't read. It needs a clean." then "Clean. Second try." `index.html?clean=1` makes every insert do it, and so does holding Shift while putting a disc in (Shift+Enter, Shift+Space or Shift+click), for trying it.

**Skipping.** Three quick clicks (within 0.9 s) anywhere in the library, or three presses of Enter, Space or Esc, skip whatever is moving (the insert, the cleaning, the eject) and the wait in the dark: on the player the dialog goes at once and the boot shows as it is.

**Motion.** The tray and the disc move (about 3 s in all); with reduced motion every duration is 0. Departures from §1, on purpose and only here: a photo, moving objects, a translucent disc hub, and the console's power light glowing. The photo carries no Sony or PlayStation marks: they were taken out of it (`tools/wasm/delogo-photo.py`).

## 16. The console under the screen

The transport (§5) is the PS2's front drawn as a wireframe after the real one (an SVG in `index.html`: a sliver of the top, the front with its grooves, the memory card slots, the controller ports, a plate, the tray, Reset and Eject; thin lines in the house's greys), as wide as the screen, with a button laid over each part (`library/console-bar.css`). Under its tray lies the TV remote of the first mockup, turned on its side with its lettering upright. A first version, to be refined with the owner.

- **Memory card slots 1 and 2:** each is Card 1 / Card 2 (§14). A plugged card is drawn like the console, the whole card (its "8MB MEMORY CARD" print, grip lines at its end) hanging from the slot's top edge, pushed in until only its end shows, filling the slot's opening; its name sits above the slot beside its number ("1 · RPG saves"). Plugging it in (from the picker, a new card or a card file) shows the whole card below the slot and slides it up into place (760 ms); unplugging pulls it back down out of the slot and it fades (620 ms). Nothing slides with reduced motion. Clicking the slot or the card opens its menu as before.
- **The blank plate** right of slot 2 is engraved with the console model from the BIOS file's name (SCPH-70004, else the BIOS's title) and opens the BIOS menu.
- **The tray** is engraved with the game's title and opens the disc menu; with no disc it reads "Insert a disc" and opens the disc library, or, while a game runs with its tray empty (§10, Swap), the disc picker: the disc goes in without stopping the game.
- **Reset** is Stop (while starting or running). Its power light glows green while the PS2 runs.
- **Eject** opens the disc library (§15); while the PS2 runs the subtitle says why it can't.
- **Controller ports 1 and 2:** while something plays as that player, a plug sits in the port and its cable leads down the page to the player's controller (§17). Pressing a port (`aria-pressed`) pulls the plug out: what played as that player stops and is remembered (the keyboard can be unplugged too). Pressing it again pushes the plug back in with what was there, if it is still free, else the keyboard or a gamepad that plays nowhere; with nothing free the subtitle says to choose on the controller. The subtitle says "Player 2 unplugged." / "Player 2 plugged in: the keyboard." A port's name and tooltip say what plays there ("Controller port 1, Player 1: Keyboard"). `?` still opens the Keys drawer (§13) for the keyboard's player. While it is open the remote steps aside, and the console is never wider than 760 px, so the picture keeps its room.
- **The remote** (276 × 84 px, overlapping the console's bottom edge under the tray): an IR window; the VOL rocker (− and +, steps of 10; the subtitle says "Volume 80%.") over MUTE (`aria-pressed`, M as before) and WIDE (Fullscreen), the two as wide as the rocker and its label; PICTURE over MENU (Graphics, §12); PICTURE cycles the picture modes, Smooth, Crisp, CRT and CRT 2 (CRT is CRT-Geom by cgwg, Themaister and DOLLS, as ShaderGlass has it, TV shader 8: the picture on a spherical tube with rounded corners, one beam per pair of PS2 lines, wider where it is bright, and an aperture mask, nothing moving; CRT 2, TV shader 9, is the same tube with the light the glass spreads around bright things, a short trail behind moving ones (the device keeps the picture accumulated over frames and a blur of it) and slightly darker corners; both applied as the WebGPU device presents and left out on the 2D path, which has no TV shaders, where the subtitle says so), the subtitle naming each ("Picture: CRT."), its amber dot on when the mode is not Smooth, kept in this browser (`pcsx2web.picture`; an earlier Crisp carries over); REC (the Recorder, when shown) over INFO (Log), INFO centred when alone. A pressed or open one has a small amber dot. The Sound menu has no button any more.
- On windows 1440 px wide and more (not in fullscreen), the remote lies beside the console, 28 px to its right and centred on it, instead of under it: the room's height goes to the television.
- The sill (capture, subtitle, readout, tally) stays above the console. In fullscreen only the remote rides on the band. Under 640 px the remote wraps under the console.

## 17. The controllers

Below the first screen (the room: marquee and theatre, as tall as the window) lie two controllers, and below them the credits, Player 1 and Player 2, under the heading "Controllers" (`library/console-pads.css`). Each is drawn as a wireframe in the console's style, after the owner's reference line drawing of the real controller seen from above (600 × 310 units, `pad-template` in `index.html`, rebuilt from simple shapes at its proportions): two round wings with plus-shaped plates for the D-pad (four keys pointing in) and the face buttons, joined by a bridge with Select (a bar), Start (a triangle) and Analog (a bar); the sticks in round housings that merge with the wings; the grips angled out; L2 and R2 as thin tabs behind L1 and R1. No maker's marks, logos or button symbols. A first version, to be refined with the owner.

- **The cable:** while something plays as a player, its port holds a plug and a cable (a 5 px tube in the console's greys, the same all the way) comes out of the boot at the plug's centre (the cable starts there, drawn in front of the page over the console's face and under the boot; no straight piece), and runs behind the page (behind the console's face, the remote and the drawers) down into the controller's strain relief, natural from end to end. Both cables are as long, as on the console, and longer than either needs (1.5 times the longer straight run, plus 115 px). Their shape is not drawn but settled by physics: each is a rope of 9 px links in matter-js (`cable-worker.js`, off the page's thread; `vendor/matter.min.js`, MIT, pinned in `pcsx2-wasm/assets`), held only at its two ends, the plug and the strain relief, stiff over a few links so it bends gently, with a memory of being coiled (a random preferred bend varying along it, and stretches stiffer than others) so that its turns are uneven rather than perfect arcs, dropped in random curves broad and small and left to settle flat on the floor between the console and the controllers, on the screen, above both controllers' drawings (pushed up, out of its own controller's shoulders and labels, with a lane down the middle to the strain relief), out of the other controller's way (sideways) and the heading's. The settings were chosen by the owner with the cable tuner (`?tune-cables`, pcsx2-wasm/README.md). Both then measure exactly the same: each rope's way out from its straight run is scaled by a hair to the common length (934.0 px each at 1280 wide) (128 px of floor above the controllers; 240 px with one under the other, where the second cable goes round the first controller). Seeded, so the same layout always lies the same way. The page sends the layout, one at a time (while the worker settles one, a newer layout replaces the waiting one, so resizing the window never queues stale layouts), and hides the cables while they lie for an older layout, until the current one's come. Plugging in, the cable comes up from the controller to the port (600 ms), then the plug goes in; pulling out, the plug comes out, then the cable slides back down to the controller. Nothing moves at load or with reduced motion. The cables fade with the controls (§5, data-chrome). A controller nothing plays on lies dimmed, with no plug, no cable and no buttons.
- **Its device:** under each controller, "Player N" and a choice (radio buttons) of the keyboard and each connected gamepad. Choosing one moves it to that port (the keyboard plays on one port at a time) and shows its controls; other devices that also play there are named under it ("The keyboard also plays as Player 1."). Unchosen, a controller shows a gamepad on its port, else the keyboard.
- **Its controls:** a button over every control shows what plays it: a key (as the layout prints it: arrows as icons, "Enter", "Bksp", AZERTY letters) or a gamepad control ("A", "LB", "×", "B3", "A1−", "H9", arrows for a gamepad's own sticks). A control nothing plays shows nothing. The shoulder buttons sit behind one another, so dashed lines lead each one out to its button beside the controller, with its name (L2 and L1 left, R1 and R2 right); they are the only controls named on the drawing. Each stick has a button per direction and its press (L3, R3) in the middle. What is held lights amber, on the keyboard and on the gamepad; a gamepad's stick caps follow its sticks.
- **Changing one:** clicking a control makes it listen (amber outline, blinking; steady with reduced motion) and the status line says what to do: "Press the key for Cross. Esc cancels." The next key plays it; the control that had that key takes this one's old key, so that no key plays two controls and none goes missing ("J plays Cross now, and K Square."). Escape, Tab, a click elsewhere or a second click cancels ("Unchanged."). Keys the browser keeps (F5, F11, F12) are refused. On a gamepad, the next button, trigger or axis is taken (a stick is mapped whole, its two axes in turn). Choices are kept in this browser: the keys in `pcsx2web.keyBinds` (the Keys drawer shows them too), a gamepad's in `pcsx2web.padMaps` with the mappings of §13. A gamepad that does not report a standard layout also offers "Map all controls". "Reset the keys", "Reset the controls" or "Clear the mapping" appears once something was changed.
- A key given to the pad no longer works as a page shortcut (M, `?`), as before.
- **Rumble:** the game's two motors (the DualShock 2's large one, variable, and small one, on or off) play on every gamepad that plays as that player, where the browser can vibrate it (Chromium's `vibrationActuator`, `dual-rumble`: Xbox, DualShock 4 and DualSense pads; Firefox has none). Under the device choice, a checkbox **Rumble** (on by default, remembered per player in `pcsx2web.rumble.<port>`) shows while the controller shows such a gamepad, with a quiet **Test** button beside it (named "Test rumble for Player 1"): the strong motor, then the weak one, 0.4 s each, on that player's gamepads whether the game's rumble is on or off; the status line says "Rumble test: the strong motor, then the weak one." Nothing rumbles while the game is not running or the page has no focus, while the disc library is open, or while a gamepad is being mapped. The page stops every motor itself (`reset()`) when it is closed, hidden or loses focus, and at the end of a test: Chromium does not always end an effect after its duration (a pad kept rumbling after its tab was closed).

## 18. The television

The screen sits in an old CRT television, drawn as a wireframe in the console's style (the SVG in `index.html`, 1080 × 952 units with the screen 1000 × 750 at 40, 64: a slim set, so that the picture keeps as much of the room as it can; `library/console-tv.css`): a deep cabinet with a sliver of its top, the tube's dark surround, and under the screen a speaker grille, a blank plate (no maker's name), a power button with its light (green while the PS2 starts or runs, like the console's), a channel dial with its ticks and a volume knob, on two feet. The screen and the slate shrink to leave the set room (the screen's width is the largest that lets the whole set fit the stage) and move up so that the set is centred; their corners round like a tube's glass. In the set the screen has no frame line (the amber one of §5 is gone: the tally and "Keys go to the PS2" under it say where keys go); keyboard focus on the screen lights the tube's surround in light grey instead. The set is decoration only (`aria-hidden`, no controls). It is not shown in fullscreen, where the picture fills the screen as before, nor under 640 px, where the screen takes the width.

## 19. Save states

A save state is a snapshot of the whole PS2 at one moment, PCSX2's `.p2s` format, so states move between TabStationToo and desktop PCSX2 of the same version. It is not a memory card save: the game never sees it, and loading one puts the console back to that moment (the memory cards stay as they are, as in PCSX2). Plain words: "save state", "state".

**STATE** (`#btn-state`, `.rm-small` on the remote, `popovertarget="state-menu"`), at the top of the remote's right column, over CLOCK and INFO (the three 27 px apart). With the Recorder shown, the remote grows a fourth column (42 px wider) and REC moves there, centred. Accessible name "Save states"; with states for the running game, "Save states, 3 for this game". Never disabled. Shortcuts as on the desktop: **F1** saves a state, **F3** loads the newest state of the running game; both only while focus is on the screen or the controls under it, and the Keys drawer lists them.

**The menu** (`#state-menu`, popover, `role="group"`, 320 px, placed like the Underclock menu, focuses its first enabled item).
- **This game:** the running game's title (or its serial), "The console's menus" while the BIOS runs alone, or "No game running".
- **Save state** (Phosphor `camera`, "F1" on the right in mono): saves at the CPU thread's next pause, then lists it first. "Saving…" while it runs (about a second; the picture pauses). Disabled while nothing runs, described by "Start a game to save its state."
- **The states** of the running game (by serial; the console's menus have their own), newest first, at most 10: a game with 10 replaces its oldest on Save state, and the fine print says so once there are 10 ("10 states: saving replaces the oldest."). Each row is a button: a thumbnail of the picture at that moment (64 × 48, 4:3, 1 px line), the time ("Today 19:42", "Yesterday 08:10", "12 Oct 21:03"), and under it "Newest · F3" for the first, otherwise the size ("14.2 MB"). Choosing a row loads it into the running game ("Loading…" on the row). A **More** button (`dots-three`, "More for the state of today 19:42") opens a view with the time, then **Save a copy to disk…** (a download, `<serial> <date> <time>.p2s`), **Delete** (no question: the copy to disk is the backup) and **Back to the states**.
- **Open a state file…** (`folder-open`): loads a `.p2s` into the running game and adds it to this game's states. Disabled while nothing runs.
- With no states for the game: "No states for this game yet." in ink-2 above Open.
- Fine print: "Kept in this browser." · "States only load into the game they were saved from, in this version of TabStationToo or one close to it."

**Storage.** IndexedDB (`pcsx2-web-states`): one record per state with the serial, the game's title, the time, the size, the thumbnail (JPEG) and the `.p2s` bytes. The page asks for persistent storage as for memory cards. A state saved while another TabStationToo tab is open still saves (states are written once, never shared live).

**Loading.** Only into a running game. The list only holds the running game's states; a file from Open a state file… says nothing of its game (as in PCSX2, a state from another game loads that game's moment over the disc, and the game usually crashes), so the picker is for this game's own copies. While the game wrote to a memory card in the last 5 s (PCSX2's busy time) PCSX2 refuses to load, to protect the card: "The game is saving to a memory card. Try again in a few seconds." A state PCSX2 cannot load (another version, a damaged file) leaves the game running as it was: "That state didn't load. It may be from another version of TabStationToo." The Log has PCSX2's reason.

**Errors** (a coral line in the menu): "Couldn't save the state. The log may say why." · "The browser has no room for another state. Delete one or save a copy to disk." · "Couldn't read that file. Try choosing it again." · "That file isn't a PS2 save state (.p2s)."

**Subtitles and announcer:** "State saved." · "State loaded: today 19:42." · "State deleted." · "No state for this game yet. F1 saves one." (F3 with none).

**Not here:** loading a state before a game boots (resume from the library), states of games without a serial in the library, and sharing states between browsers other than by file.
