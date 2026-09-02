## LSP diagnostics

- Live firmware file: `lsp_diagnostics` could not run because the LSP tool is scoped to the main request cwd and rejected `/Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c` as outside cwd.
- Mirrored bundle file: clang diagnostics are not authoritative from the main repo because Flipper SDK headers are not on the LSP include path (`'furi.h' file not found`, followed by cascading unknown-type errors). The transient duplicate macro/member diagnostics from the first failed build were fixed.
- Meaningful C gate: `/Users/asutov/projects/flipperzero-firmware ./fbt build APPSRC=applications_user/pocket_airbridge` passed after the duplicate fix; see `fbt-build.log`.
