# Pocket AirBridge Design System

## 1. Atmosphere & Identity

Pocket AirBridge is an **AirBridge Relay Console**: a deliberately hackerish,
Linux-terminal-like workspace for an offline encrypted relay. Its signature is a
near-black shell assembled from hard-edged graphite panes, 1px grid gaps, and
text-first prompts. Signal mint identifies a verified connection; amber is only
for pending attention; red is reserved for an actual abort or failure. It never
pretends to be a real shell: command-shaped controls are semantic browser
controls, and the UI never accepts arbitrary commands.

## 2. Color

### Palette

| Role | Token | Value | Usage |
|---|---|---:|---|
| Canvas | `--ab-canvas` | `#0c0c0c` | Shell and page background |
| Surface | `--ab-surface` | `#1a1a1a` | Primary pane |
| Surface inset | `--ab-surface-inset` | `#111111` | Transcript, log, inputs |
| Surface raised | `--ab-surface-raised` | `#202020` | Hovered/selected row |
| Ink | `--ab-ink` | `#f0f3ee` | Primary text |
| Ink muted | `--ab-ink-muted` | `#a8b1a8` | Metadata and help |
| Rule | `--ab-rule` | `#333333` | 1px pane separation |
| Rule strong | `--ab-rule-strong` | `#5b665b` | Control outlines |
| Signal | `--ab-signal` | `#9eea6a` | Verified/connected actions |
| Signal active | `--ab-signal-active` | `#c4ff92` | Hover/focus signal |
| Pending | `--ab-pending` | `#e0ae4f` | SAS and non-failure attention |
| Failure | `--ab-failure` | `#f06c62` | Failure, abort, cancellation |

### Rules

- No decorative color gradient, glass, drop shadow, or branded radio imagery.
- Color augments clear text and shape; no state is color-only.
- The sole optional atmosphere is a static, low-contrast scanline/noise texture
  outside readable panes.

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

- The `scroll-body-shell` has a persistent console rail and a single document
  scroll owner; `100dvh`, never `100vh`, bounds full-height shells.
- At wide widths, the workspace is a three-column console: a 17.5rem
  connection/security rail, a dominant fluid transcript workspace, and a 17.5rem
  transfer/log rail. Its maximum inline size is `--ab-content-max` (90rem).
- At `--ab-bp-compact` (64rem), the three regions collapse to one task-ordered
  column: connection/security, chat workspace, then transfer/log diagnostics.
  At `--ab-bp-narrow` (44rem), this remains the single readable column.
- Long device names, filenames, hashes, logs, and errors use
  `overflow-wrap: anywhere`; 375px has no primary-content horizontal scrollbar.

## 5. Components

### Console rail
- **Structure:** product mark, endpoint label, connection/security facts, action cluster.
- **States:** disconnected, connecting, connected, locked, SAS pending, verified,
  aborted.
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
- **States:** default, hover, active, focus-visible, disabled, busy.
- **Accessibility:** native labels, keyboard operation, 2px focus outline, and
  no destructive-looking command language.
- **Motion:** 120ms color/opacity/transform feedback; reduced motion removes
  transforms.

### Transcript row and attachment card
- **Structure:** endpoint prompt (`you@usb>` or `peer@ble>`), timestamp, body or
  file facts, verification, segmented progress, and download/remove controls.
- **States:** hashing, sending, receiving, verifying, ready, cancelled, failed.
- **Accessibility:** phase attributes and existing live/progress semantics remain
  unchanged; names and hashes wrap rather than vanish.

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

Only `transform`, `opacity`, `color`, and `filter` animate. The optional scanline
texture is static; `prefers-reduced-motion` disables non-essential transforms.
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
- The console's dense technical language is paired with direct recovery wording;
  critical transfer state is never hover-only or dependent on memory.
- Personas: keyboard-first operator; low-vision operator at 200% zoom; distracted
  operator who must distinguish pending, verified, and failed transfer states.

### Accepted Debt

| Item | Location | Why accepted | Owner / Exit |
|---|---|---|---|
| Native offline monospace stack | Both chat endpoints | No external font may add a network dependency or inflate the bundle | Replace only with an in-tree licensed font and new bundle proof |
| Static atmosphere only | Shared stylesheet | Heavy CRT/VHS motion would impair readability and distract from transfer state | Keep unless user requests a tested accessible alternative |
