# Open work

This is a backlog, not an implementation inventory or release schedule.
Existing wlroots Xwayland support, clipboard/primary-selection transport,
keyboard de-duplication, pointer gestures, and the application launcher need
regression coverage and polish; they are not described here as absent.
See [Architecture](docs/architecture.md) for current behavior and
[HEVC diagnostics](docs/video-hevc-troubleshooting.md) for media troubleshooting.

## High priority: compositor correctness

- **Popup and window management:** finish popup constrain/reposition behavior,
  compositor-initiated close, activation/focus transitions, transient/modal
  ordering, and toplevel tiled/suspended/bounds bookkeeping.
- **Xwayland integration:** polish clipboard bridging and keyboard grabs on top
  of the existing Xwayland startup support.
- **Keyboard:** exercise Ctrl/Alt/Super/Shift combinations, keymap and modifier
  state during enter/leave, duplicate repeat handling, shortcuts inhibition,
  and dropped/injected event counters.
- **Pointer:** verify enter/leave/frame semantics; add relative pointer,
  pointer constraints, and locked/confined pointer behavior.
- **Clipboard:** exercise primary-selection ownership and client/server
  conflict cases; assess additional MIME types and data-control support.

## Medium priority: display and media

- **Client-side cursor:** transmit shape/hotspot, render locally, and retain a
  fallback when the client cannot represent the cursor.
- **Presentation-time and scaling:** add presentation-time, fractional-scale
  negotiation, preferred scale, and viewport/buffer-scale validation. Extend
  output scale/transform behavior alongside that work.
- **linux-dmabuf:** investigate compositor/client import paths while preserving
  correctness when readback remains necessary. No zero-copy claim until tested.
- **Other compositor protocols:** idle-inhibit and single-pixel-buffer.
- **HEVC performance:** keep the repaired VA-API Annex-B stream and audio-sync
  behavior covered by integration tests; profile sustained FPS, encoder queue
  pressure, and driver-specific format handling. Software HEVC can remain below
  the requested session FPS.

## Later: shell and platform integration

- Color management and HDR.
- Additional xdg-toplevel tagging, dragging, and shell niceties.
- Security-context, portal-adjacent integration, and sandbox behavior.
- Authentication/encryption must be designed together before any deployment on
  an untrusted network; see [Security](SECURITY.md).
