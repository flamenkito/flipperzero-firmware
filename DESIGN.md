# Pocket AirBridge Design System

## 1. Atmosphere & Identity

Pocket AirBridge is an **AirBridge Relay Console**: a deliberately hackerish,
Linux-terminal-like workspace for an offline encrypted relay. Its signature is a
flat near-black vim/neovim-style shell assembled from hard-edged graphite panes, 1px rules, and
text-first prompts. Signal mint identifies a verified connection; amber is only
for pending attention; red is reserved for an actual abort or failure. It never
pretends to be a real shell: command-shaped controls are semantic browser
controls, and the UI never accepts arbitrary commands.

## 2. Color

### Palette

| Role | Token | Value | Usage |
|---|---|---:|---|
| Canvas | `--ab-canvas` | `#0c0c0c` | Flat terminal shell and page background |
| Surface | `--ab-surface` | `#1a1a1a` | Primary pane |
| Surface inset | `--ab-surface-inset` | `#111111` | Transcript, log, inputs |
| Surface raised | `--ab-surface-raised` | `#202020` | Hovered/selected row |
| Ink | `--ab-ink` | `#f0f3ee` | Primary text |
| Ink muted | `--ab-ink-muted` | `#a8b1a8` | Metadata and help |
| Ink faint | `--ab-ink-faint` | `#6f766c` | Disabled ink |
| Rule | `--ab-rule` | `#333333` | 1px pane separation |
| Rule strong | `--ab-rule-strong` | `#5b665b` | Control outlines |
| Signal | `--ab-signal` | `#9eea6a` | Primary action, active or verified status, and one compact success marker |
| Signal active | `--ab-signal-active` | `#c4ff92` | Hover/focus signal |
| Pending | `--ab-pending` | `#e0ae4f` | SAS and non-failure attention |
| Failure | `--ab-failure` | `#f06c62` | Failure, abort, cancellation |

### Rules

- No decorative color gradient, glass, drop shadow, or branded radio imagery.
- Color augments clear text and shape; no state is color-only.
- The canvas is a flat dark fill: no graph-paper grid, repeating gradients,
  scanlines, noise, or other decorative textures, including outside readable panes.
- Mint is reserved for a primary action, active or verified status, and one compact
  success marker. Pane captions, routine log lines, hashes, download links, and
  inactive state labels use neutral ink.

## 3. Typography

### Scale

| Level | Token | Size | Weight | Usage |
|---|---|---:|---:|---|
| Display | `--ab-type-display` | clamp(1.25rem, 2.6vw, 1.75rem) | 700 | Product name |
| Heading | `--ab-type-heading` | 0.875rem | 700 | Pane labels |
| Body | `--ab-type-body` | 0.875rem | 500 | Messages and controls |
| Meta | `--ab-type-meta` | 0.75rem | 600 | Facts and timestamps |
| Micro | `--ab-type-micro` | 0.6875rem | 650 | Tags and state rail |

### Font Stack

- Primary and mono: `ui-monospace, "SFMono-Regular", Menlo, Monaco, Consolas, "Liberation Mono", monospace`.
- Numbers use tabular figures. No externally loaded font is permitted because the
  product is browser-only/offline and the USB bundle is size-sensitive.
- Prompt labels use uppercase tracking; user-written message text stays sentence
  case and wraps naturally.

## 4. Spacing & Layout

### Base Unit

Spacing uses a 4px base: `--ab-space-1` (4px), `--ab-space-2` (8px),
`--ab-space-3` (12px), `--ab-space-4` (16px), `--ab-space-5` (20px),
`--ab-space-6` (24px), and `--ab-space-8` (32px).

### Shell

- The operator chrome is a compact tabline followed by a statusline, each a single
  14px to 16px bar. The tabline identifies `[airbridge]` and its endpoint, and may
  show the `MODE=mock` segment. The statusline uses space-separated `KEY=value`
  fields, for example `USB=connected SEC=verified SAS=729981 ✓ state=ready`.
- `MODE=mock` is an accessible disclosure with invocation help available through
  keyboard and touch operation.
- The `scroll-body-shell` has a persistent console rail and a single document
  scroll owner; `100dvh`, never `100vh`, bounds full-height shells.
- At wide widths, the workspace is a three-column console: a 17.5rem
  connection/security rail, a dominant fluid transcript workspace, and a 17.5rem
  transfer/log rail. Its maximum inline size is `--ab-content-max` (90rem).
- At `--ab-bp-compact` (64rem), the three regions collapse to one task-ordered
  column: connection/security, chat workspace, then transfer/log diagnostics.
  At `--ab-bp-narrow` (44rem), this remains the single readable column.
- Long device names, filenames, hashes, logs, and errors use appropriate
  truncation or wrapping without a 375px primary-content horizontal scrollbar.
  Log records remain single-line rows and make their complete text available.

## 5. Components

### Console rail
- **Structure:** tabline/statusline operator chrome, compact connection/security
  facts, and an action cluster. Pane captions are `[connection]`, `[transcript]`,
  `[transfer]`, and `[log]`; persistent per-pane shell prompts are not used.
- **States:** disconnected, connecting, connected, locked, SAS pending, verified,
  aborted.
- **Status ownership:** `#panelState.dataset.phase` remains the authoritative
  transfer phase, using its existing values. It is separate from the derived
  statusline display state. The display-state renderer has sole ownership of the
  statusline node and applies this precedence: disconnected is `idle`; connected
  and crypto locked is `locked`; connected and unlocked with transfer phase
  `Ready` is `ready`; otherwise display the active transfer phase in lowercase.
- **Accessibility:** semantic header and text status alongside the square LED;
  focus is never hidden.
- **Motion:** status state changes only; no decorative pulse.

### Instrument pane
- **Structure:** prompt-prefixed header, information body, optional action or
  `tree`-style diagnostic block.
- **Variants:** connection, crypto, transcript, transfer, log, mock disclosure.
- **States:** default, pending, verified, failed, empty.
- **Accessibility:** headings remain real headings; failure is announced by the
  existing alert region.

### Terminal control
- **Structure:** a native button, input, or file control rendered as a square
  command affordance with a concise label such as `[ CONNECT USB ]`.
- **Tiers:** literal CSS class `btn-primary` uses `--ab-signal` ink for the
  forward action of a cluster: Connect, Send, Send Attachment, Accept SAS.
  The secondary tier uses default `--ab-ink` ink: Disconnect, Clear, Remove.
  Literal CSS class `btn-danger` uses default `--ab-ink` ink at rest and
  `--ab-failure` ink and border on hover/focus/active: Abort Crypto, Cancel Transfer.
- **States:** default, hover, active, focus-visible, disabled, busy.
  CSS-generated `[ ]` brackets form the resting enclosure; a 1px border appears
  on hover/focus/active. Disabled controls use `--ab-ink-faint` at full opacity,
  never an opacity wash; disabled styling takes precedence over tier feedback.
- **Dimensions:** uniform `2.5rem` minimum block size; `.clear-btn` uses the
  small tier at `2rem` minimum block size.
- **File selector:** `::file-selector-button` uses the small secondary tier
  (`2rem` minimum block size), with a solid `--ab-rule` enclosure rather than
  brackets because pseudo-elements cannot nest. The native selector and its
  button share the command family's height, surface, ink, and 1px border
  vocabulary.
- **Accessibility:** native labels, keyboard operation, 2px focus outline, and
  no destructive-looking command language.
- **Motion:** 120ms color/opacity/transform feedback; reduced motion removes
  transforms.

### Transcript row and attachment card
- **Structure:** full-width rows, never right-aligned bubbles, with a 2px leading
  rule colored by side: you = `--ab-signal`, peer = `--ab-rule-strong`,
  system = `--ab-pending`. Real-text prompt labels (`you@usb>`, `peer@ble>`,
  `system>`) are never replaced by a zero-font-size swap. Include timestamp, body or
  file facts, verification, segmented progress, and download/remove controls.
- **States:** hashing, sending, receiving, verifying, ready, cancelled, failed.
- **Buffer behavior:** one lifecycle-owned transcript scroll controller exists
  per transcript. It owns pinned state, unread count, resize observation, and
  disposal. When a pinned buffer appends or resizes, it re-pins at the bottom;
  otherwise it preserves the visible records and exposes `↓ N new` outside the
  transcript's `aria-live` region. Click, manual return to bottom, and clear
  reset the count. Observers and listeners dispose on teardown or detachment,
  including detached synthetic hosts.
- **Empty state:** an empty transcript is the first top-left
  `.message.system.empty` terminal row, with `system>` as its sender. It is not a
  centered placeholder.
- **Attachments:** the default card summarizes name, size, short `sha256:` hash,
  and verification. A closed-by-default details control contains the full hash in
  de-emphasized text. The complete hash remains in card text and the card dataset
  even while its visual detail is collapsed.
- **Accessibility:** phase attributes and existing live/progress semantics remain
  unchanged. Names remain available, and complete hashes remain available through
  card text and the collapsed details control.

### Activity log
- **Structure:** discrete, single-line rows with pinned-scroll behavior. A pinned
  log follows appended records; an unpinned log preserves the operator's place.
  `↑ N earlier` sits outside the scrolling record list, reports hidden earlier
  rows, and exposes them by keyboard or pointer activation.
- **Text availability:** clipped rows retain their full message through an
  accessible text alternative such as a title attribute. Hashes do not wrap
  mid-record.

### Status LED and tag
- **Structure:** square marker plus plain-language state label.
- **States:** online, pending, offline, failed.
- **Accessibility:** never color-only; hard edges distinguish a tag from a
  button.

## 6. Motion & Interaction

| Type | Duration | Usage |
|---|---:|---|
| Feedback | 120ms | Press, hover, focus |
| State | 180ms | Status and progress state change |

Only `transform`, `opacity`, `color`, and `filter` animate.
`prefers-reduced-motion` disables non-essential transforms.
Transfer lifecycle changes, focus, and progress are the only meaningful motion.

## 7. Depth & Surface

**Strategy: borders-only with tonal shift.** Console regions meet through 1px
`--ab-rule` separation and graphite surface steps. Controls and cards are square
or nearly square. A restrained signal glow may appear on an active focus or
verified state, but never as a general shadow or decorative animation.

## 8. Accessibility Constraints & Accepted Debt

### Constraints

- WCAG 2.2 AA target: 4.5:1 body-text contrast, visible keyboard focus, semantic
  labels, and status text independent of color.
- The connection/SAS/send/cancel/recovery journey must be keyboard reachable, at
  200% zoom, with reduced motion, and at a 375px viewport.
- At narrow widths, the composer uses two compact rows: `[input][Send]`, then
  `[file selector][Send Attachment]`, with a single terse helper line. The file
  selector remains a coherent native control rather than a mismatched enclosure.
- Scroll indicators remain outside live regions so unread-count changes do not
  interrupt transcript or log announcements. Mock-mode help, transcript jumps,
  and log-history controls are keyboard and touch accessible.
- The console's dense technical language is paired with direct recovery wording;
  critical transfer state is never hover-only or dependent on memory.
- Personas: keyboard-first operator; low-vision operator at 200% zoom; distracted
  operator who must distinguish pending, verified, and failed transfer states.

### Accepted Debt

| Item | Location | Why accepted | Owner / Exit |
|---|---|---|---|
| Native offline monospace stack | Both chat endpoints | No external font may add a network dependency or inflate the bundle | Replace only with an in-tree licensed font and new bundle proof |
| CSS-generated brackets | Terminal controls | Presentational `[ ]` brackets may be announced by some screen readers; native text labels remain meaningful | UI maintainer / replace with aria-hidden markup if assistive-technology testing finds disruptive announcements |
