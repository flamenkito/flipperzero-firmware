# Decisions — ui-functional-consolidation

## 2026-08-28 Locked decisions (from plan, do not re-open)
1. Shared assets web/airbridge-ui.css + web/airbridge-ui.js; no inline <style> remains in either chat page.
2. USB page grid layout (minmax(0,1fr) 280px sidebar) is THE layout for both.
3. Neutral dark palette, ONE accent, semantic ok/warn/error only. No glow, no gradient fills, border-radius <= 6px, no card shadows.
4. Connect buttons read exactly "Connect USB" / "Connect BLE".
5. Explicit "Send Attachment" on both pages; BLE auto-send on change removed.
6. Per-message attachment card is the primary progress display. Transfer State panel = one plain state line + throughput line + Cancel button. Global progress bar removed from both pages.
7. Throughput: EMA over chunk-completion intervals; "--" until measurable.
8. Timestamp HH:MM:SS 24h; hash 12 chars + ellipsis; formatBytes 1 decimal below 10 KB else 0; no emoji.
9. Connection card collapses to one status line when connected; BLE picker hint hidden once connected.
10. Clear buttons for transcript and status log on both pages.
11. build_bundle.py extended to inline airbridge-ui.js (IIFE pattern) and airbridge-ui.css (<link> replaced by <style> block).
12. Protocol/transport/identity/bootstrap/harness files untouched.
