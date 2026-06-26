# WayDisplay roadmap


0. Basic popup constraints/reposition
Partially done; keep a smaller follow-up item for correctness polish.

1. Xwayland support

Still probably the biggest compatibility multiplier.

Subtasks:
Later: clipboard bridge and keyboard grabs.

Priority: High

2. Keyboard correctness

This is probably the next best compositor-behavior investment.

Subtasks:

Verify modifier state injection: Ctrl/Alt/Super/Shift combinations.
Make sure keyboard enter/leave sends correct keymap and modifier state.
Avoid duplicate key repeat between client and compositor.
Add better logging/counters for dropped/injected key events.
Respect active keyboard-shortcuts-inhibit when adding compositor shortcuts later.

Priority: High

3. Pointer protocol polish

Needed for games, CAD, remote desktop feel, and browser/toolkit correctness.

Subtasks:

Clean pointer enter/leave/frame semantics.
Add relative pointer.
Add pointer constraints.
Add locked pointer / confined pointer behavior.
Make local cursor rendering work well with these.

Priority: High

4. Window-management behavior

This will improve normal app behavior a lot.

Subtasks:
Compositor-initiated close.
Better activated/focused state transitions.
Modal/transient/dialog focus ordering.
Toplevel bounds / suspended / tiled bookkeeping.

Priority: High

5. Remote clipboard sync polish

Clipboard exists, but for a Waypipe-like tool this should become a top-tier UX feature.

Subtasks:
Confirm primary selection ownership edge cases.
Add client/server conflict handling.
Add MIME type support beyond plain text, if not already present.
Later: data-control for clipboard managers.

Priority: High
Medium-priority compositor protocols
6. presentation-time

Useful for smooth toolkits, video, games, animation, and latency measurement.

Priority: Medium
7. fractional-scale + viewporter polish
You already have viewporter wiring, but modern apps care about fractional scale.

Subtasks:
Add fractional-scale protocol.
Set preferred scale.
Verify buffer scale / viewport interactions.
Make output scale configurable.

Priority: Medium
8. idle-inhibit
Useful for video players, presentations, remote desktop sessions.

Priority: Medium
9. single-pixel-buffer
Small compatibility/optimization protocol. Low risk, likely easy.

Priority: Medium-Low
10. data-control
Useful for clipboard managers and future remote clipboard sync.


Priority: High
12. Cursor rendered locally
Big perceived latency win.

Subtasks:
Send cursor shape/hotspot separately.
Render cursor on client.
Avoid waiting for cursor changes through frame tiles.
Fall back to frame cursor when client-side cursor is unavailable.


Priority: Medium-High
GPU / media path
15. linux-dmabuf
Important, but I’d do this after more behavior correctness unless Electron/browser/video compatibility is the immediate goal.
Even if you still read back for transport, supporting dmabuf helps clients avoid worse fallback paths.

Priority: Medium-High
16. Color management / HDR
Useful eventually, not now.


Priority: Medium
18. Output scale / transform
Do alongside fractional scale / viewport work.

Priority: Medium
Nice-to-have shell integration
19. xdg toplevel tag / drag / extra shell niceties
Good to keep, not urgent.

Priority: Low-Medium
20. security-context / portals-adjacent protocols
Useful later for Flatpak/sandbox integration, but not a core compositor blocker yet.

