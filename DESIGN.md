# DevOpsRadio Design System

## 1. Atmosphere & Identity

DevOpsRadio is a **CRT broadcast terminal**: a fullscreen monochrome console that
pretends to be a running process (`#!/usr/bin/env radio`, PID, `$ ./controls`,
`git:(main) ✓ nothing to commit`). Where a disciplined ops console suppresses
texture, DevOpsRadio *leans into* analog decay — ASCII rain, VHS noise, scanlines,
interference bands, glitch lines, CRT flicker, and a phosphor-decay oscilloscope.
The fiction is a slightly broken TV tuned to a radio station: the signal is alive,
the medium is degrading, and the green phosphor never quite sits still.

The mood serves the product: ambient focus music for ADHD developers. The noise
floor is visual stim; the controls are dead simple; nothing demands attention.

Key characteristics:
- Pure terminal vocabulary: shebang header, shell-prompt panel titles (`$ ./controls`,
  `$ ./radio --status`), tree output (`├─ └─`), ASCII checkboxes `[_]`, keycap chips
- Phosphor green (`#00ff00`) as the single live-signal color; red only for LIVE/mute
- Analog decay as identity: noise, rain, flicker, and glitch are features, not bugs
- One primary action in the entire product: the play button. Everything else is trim
- Keyboard-first: `SPC` play/stop, `M` mute, `K` keyboard ASMR, `C` cafe ambience

## 2. Color

### Palette

| Role | Token | Value | Usage |
|---|---|---:|---|
| Canvas | `--void-black` | `#0c0c0c` | Page background, panel fill, waveform area |
| Elevated | `--elevate-black` | `#1a1a1a` | Header, footer, control bars, volume track, keycaps |
| Ink | `--pure-white` | `#ffffff` | Primary text |
| Muted | `--muted` | `#666666` | Labels, comments, secondary values, links |
| Border | `--border` | `#333333` | 1px rules, panel hairlines, idle outlines |
| Signal | `--success-green` | `#00ff00` | Live values, prompts, play affordance, EQ, shebang |
| Failure/Live | `--error-red` | `#ff4444` | REC/LIVE indicator, muted state, playing border |
| Warning | `--warning-yellow` | `#ffff00` | Reserved (declared, currently unused) |

### Rules

- Green means *signal*: values that are live, true, or actionable. Never decoration-only
- Red means *hot*: the stream is LIVE (REC dot, playing border) or audio is muted
- Pure white is prose; muted gray is metadata. No intermediate grays
- No brand color, no gradients on surfaces, no imagery — color count stays at seven
- State is never color-only: LIVE has a blinking dot + text, mute has `[M]` + red,
  checkboxes flip `[_]` ↔ `[x]` glyphs in addition to color
- State/liveness is conveyed by text + glyph; color is a secondary amplifier —
  values are white by default and green only when live/true/actionable

## 3. Typography

### Scale

| Token | Size | Usage |
|---|---:|---|
| Micro | 0.6rem (9.6px) | EQ label, freq readout, keycaps |
| Meta | 0.65–0.7rem | Volume labels, panel titles, footer, status output |
| Body | 0.75rem (12px) | Header, checkboxes, play status |
| Status | 0.8rem | STATUS / LISTENERS / UPTIME line |
| Icon | 2.5rem | Play overlay ▶ glyph |

### Font Stack

```css
--font-mono: 'IBM Plex Mono', 'JetBrains Mono', monospace;
```

- IBM Plex Mono 400/500/600 primary; JetBrains Mono 400/500/700 fallback
- Applied with `* { font-family: var(--font-mono) !important; }` — total mono hegemony,
  no proportional escape hatch anywhere
- `font-variant-numeric: tabular-nums` on the stream clock
- Letter-spacing as texture: 1px on volume fills/EQ, 2px on play status

## 4. Spacing & Layout

### Base Unit

No formal scale; spacing is ad-hoc 4px multiples — 8/12/16/24/48px recur.

### Shell

- Fullscreen grid: `grid-template-rows: auto 1fr auto`, `min-height: 100vh`,
  hard floor `min-width: 320px`
- **Header** — 12px 24px padding, 0.75rem, elevated black, 1px bottom rule.
  Left: shebang + comment. Right: PID, version, `?` help button
- **Main** — `grid-template-columns: 280px 1fr 280px` with `gap: 1px` over a
  `--border`-colored track, so the grid gap itself paints the hairline dividers
- **Left panel** `$ ./controls` — volume bar (ASCII fill over hidden range input),
  ambient-layer checkboxes, conditional ASMR volume, SPECTRUM EQ pinned to the bottom
- **Center panel** — waveform play button (max-width 600px) + status triplet
- **Right panel** `$ ./radio --status` — tree output of stream metadata, keybindings grid
- **Footer** — 10px 24px, elevated black, top rule: telegram link, credits, git status
- Help is a fixed bubble (`top: 50px; right: 24px; max-width 360px`), not a modal

### Breakpoints

- `≤1024px` — main collapses to one column; panels become wrapped rows; panel
  titles hidden; each section min-width 200px
- `≤600px` — header stacks vertically; keybindings hidden; third status item
  (UPTIME) hidden; footer stacks

## 5. Components

### Play button (the product)

- Full-width bordered tile: waveform canvas (80px) on top, control bar below
- Idle: ▶ overlay at 70% black scrim, green glow (`text-shadow` 20–30px), border `--border`
- Hover: green border + `box-shadow: 0 0 30px rgba(0,255,0,0.15)` + chromatic
  aberration on the label (red/cyan ±1px split)
- Playing: border flips `--error-red`, hover glow flips red, overlay fades out
- Control bar: REC dot (8px, blink 1s) + `LIVE` + play status left; tabular clock right
- `:active { transform: scale(0.99) }` — the only press feedback

### Volume control

- ASCII bar (`█` fill) rendered over a transparent native `<input type=range>` —
  the terminal is the skin, the browser control does the work
- Label row: `VOLUME: n%` + `[M]` mute chip; muted state turns the chip red
- Character width measured offscreen and cached (`measureCharWidth`), cache
  busted on resize

### Progress bar (ASCII fill)

- Elevated-black container with a 1px `--border`; green `█` fill at 0.65rem with
  1px letter-spacing, repeated to the measured content width and current fraction
- Label row pairs muted text with a green value; one offscreen character measurement
  is cached per class and busted on resize
- Interactive variants overlay a transparent native range input; read-only variants
  omit it

### Buttons

- Transparent or elevated-black tiles use a 1px `--border` and muted text; state is
  border + text only: primary hover is signal green, neutral hover white, danger red
- No filled buttons, solid-color backgrounds, or green squares; the sole glow is the
  primary action hover halo (`0 0 30px rgba(0,255,0,0.15)`)
- `:active { transform: scale(0.99) }` supplies press feedback; keyboard hints use
  keycap chips

### Command line

- The terminal fills the chat card without a panel title; its bottom line is a
  borderless `>` prompt with no placeholder or native input caret. One green `_`
  cursor blinks after the typed text at all times; the real input retains value,
  selection, copy, and paste while its glyph paint is transparent. A same-font,
  same-padding mirror overlay renders the visible text and measures its width so the
  cursor tracks typing, focus changes, horizontal scrolling, and resizing
- Mirror syntax is white for messages, `//` literal escapes, and proper command
  prefixes; exact commands use signal green and unknown command tokens use failure red
- A fish-style completion row inserts a muted candidate suffix after the cursor and a
  keyboard list above the prompt shows `/name — description`; `Tab` accepts the longest
  common prefix (or the full unique match), arrows cycle, and `Esc` dismisses
- One command table owns each command name and short description and is consumed by
  parsing, completion, and `/help`; aliases such as `/attach` remain table metadata
- Keyboard-first: typing anywhere focuses the prompt
- The prompt is a fixed two-line terminal block: context always occupies its reserved first line, input always occupies the second, and transcript scrolling absorbs state/content changes
- A compact, decorative shell-context line sits immediately above the prompt and
  derives from the canonical right-rail state: `usb/webhid` or `ble/web-bt`, then
  connection, crypto verification, SAS, and `(mock)` when applicable
- Live connection and verified state use signal green (`● connected`, `✔ verified`),
  connecting/locked/pending use warning yellow, and failures use error red; glyphs
  and words keep every state legible without color
- Disconnected context is muted and ends with the actionable hint
  `○ disconnected — /connect to begin`; detailed pinned status remains in the
  right-panel tree while redundant header status and mode readouts stay hidden
- Transcript and composer are one continuous canvas with matching horizontal padding,
  no separating rule, form fill, or input box; attachment-limit helper copy is absent
- Enter submits plain text as a message and leading-slash input as a
  case-insensitive command; `//text` escapes command parsing and sends `/text`
- Every command echoes to the log; `/help` lists available commands there without
  adding system messages to the transcript
- `/file` always opens attachment selection when no transfer is active (`/attach` is an
  undocumented alias). A connected, verified endpoint sends the selection immediately;
  disconnected or unverified selections remain staged as terminal lines above the prompt,
  explain that connection and verification are required, and send on Enter after verification.
  `/logs` toggles the right-rail log
- File selection immediately activates a live Hashing panel on the sender; its byte
  counter and ASCII bar advance during the incremental digest pass. The receiver activates
  an `awaiting meta…` Receiving state as soon as protocol HELLO arrives; HELLO follows the
  digest pass because encrypted metadata includes the completed plaintext SHA-256
- No action buttons are visible; wired DOM buttons remain only as hidden programmatic
  fallbacks for tests and integrations

### Ambient checkboxes

- Native checkbox hidden; visual is a text glyph `[_]` → green `[x]` (implied by
  `input:checked + .checkbox-visual`)
- Toggling KEYBOARD_ASMR reveals a second independent volume slider

### Status output

- Tree glyphs (`├─`/`└─`) list stream, source, format, channels, state
- Keys are lowercase plus a colon (`stream:`, `state:`) and muted; values carry
  color: pure white by default, signal green only for `.value.active`
- State stays legible in text; green only amplifies live/true/actionable values
- Horizontal status bars place `├─ key: value` segments inline, end with `└─`,
  and terminate in a green blinking cursor `_` (1s blink); narrow views may stack
  the same tree lines
- Tree glyphs and cursors are decorative (CSS-generated or `aria-hidden` spans),
  preserving machine-readable text contracts
- Headers pair a shebang-style green wordmark with a muted comment; right-side
  readouts use `key: value` with green values

### Keybindings

- Two-column grid of keycap chips (`SPC M K C`): elevated black, 1px border,
  0.6rem, min-width 20px

## 6. Motion & Interaction

- UI transitions: `all 0.2s` (chips, links, checkboxes), `0.3s` (play button, overlay)
- Ambient loops: CRT flicker 4s on main, noise-shift 0.2s `steps(10)`, interference
  scroll 8s linear, ASCII rain via 50ms canvas interval, REC/cursor blink 1s
- Glitch lines and static bursts are probability-driven (`Math.random()` gates),
  so decay feels organic, not scheduled
- Oscilloscope draws with phosphor decay (low-alpha black fill per frame)
- No `prefers-reduced-motion` handling — accepted debt (see §8)

## 7. Depth & Surface

Depth is **simulated analog damage**, not elevation:

- Five stacked fullscreen layers: ASCII rain canvas (z −1, opacity 0.3), white-noise
  canvas (z 996, 0.015), interference bands (z 997), VHS noise (z 998, 0.04, SVG
  `feTurbulence`), glitch lines (z 998), scanlines overlay (z 1000)
- Panels themselves are flat: no shadows, no radius, 1px rules only
- The single allowed glow is the play button's signal halo

## 8. Accessibility Constraints & Accepted Debt

### Constraints

- Full keyboard operation with documented bindings (SPC/M/K/C)
- State never conveyed by color alone (glyph + text + color everywhere)
- Real form controls under the skins: native range inputs and checkboxes drive the
  ASCII visuals
- Green on void-black is ~15:1; white on void-black is ~19:1 — primary content far
  exceeds WCAG AAA
- Layout floor at 320px; content reflows at 1024px and 600px without horizontal scroll

### Accepted Debt

- `--muted #666666` on `#0c0c0c` ≈ 2.4:1 — secondary text fails WCAG AA; the
  aesthetic price of a dim terminal
- Micro text at 9.6px (EQ label, keycaps, freq readout) below the 12px floor
- No `prefers-reduced-motion` query — flicker, noise, rain, and glitch always run,
  which is at odds with the ADHD-friendly mission and the top candidate for a fix
- No visible `:focus-visible` styling — keyboard users rely on default agent outline
- `font-family !important` on `*` blocks user font overrides
- Fixed-position help bubble + scanline overlay (z 1000) sit above everything;
  focus order into the bubble is not managed
