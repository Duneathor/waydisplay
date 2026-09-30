# Architecture

## System shape

WayDisplay has two processes:

- `waydisplay-server` runs a headless wlroots compositor, captures damage, chooses tile or video transport, captures audio, and accepts client input.
- `waydisplay-client` receives media and state, performs decode and reassembly, presents through SDL, and sends local input and clipboard updates.

The protocol is version `2`. There are no compatibility guarantees while the software remains undeployed. For media failures, use [HEVC diagnostics](video-hevc-troubleshooting.md) and the interval health/cadence counters; there is no per-frame packet trace.

## Design priorities

Architecture decisions are evaluated in this order:

1. latency
2. throughput
3. graphical correctness
4. audio correctness
5. security

Late work is often less useful than dropped work. Queues are bounded, obsolete generations are discarded, and telemetry is observational rather than authoritative.

## Common layer

`src/common` and `include/waydisplay` own shared primitives:

- time and logging
- socket helpers and incremental TCP framing
- protocol wire codecs and typed dispatch contracts
- tile and selection formats
- compression helpers

Protocol v2 sends one-byte-packed, fixed-width C wire structures directly. The implementation therefore requires little-endian Linux hosts and GCC-compatible packing; compile-time size assertions define the current wire ABI, and there is no cross-version or cross-ABI compatibility promise. liburing is mandatory, and the compositor targets the wlroots 0.20 ABI explicitly. The transport probes `send`, `sendmsg`, `recv`, and `async_cancel` at ring creation and intentionally avoids operations introduced after Linux 5.14.

## Server

The server is presented externally as an opaque `wd_server`. Private interfaces divide responsibility among:

- compositor and wlroots integration
- network connection lifecycle
- input and clipboard routing
- stream scheduling and transport
- video pipeline work
- audio capture and packetization
- telemetry aggregation

The server may assume at least four logical CPUs. Network progress, tile compression, and video encoding must not share an unconstrained work queue.

## Client

The client may run on only two logical CPUs. Its preferred runtime shape is:

- the SDL/render thread
- one receive/network worker polling all established channels
- asynchronous io_uring transmit queues serviced without permanent sender threads
- bounded decoder-internal threading where required

Transport/session ownership is centralized. Protocol handling, render planning,
reassembly, synchronization, and telemetry are kept independent from SDL where
practical. Dependency-light policy stays in small C or header-only helpers;
backend wrappers may remain C++ where they own SDL or codec objects. Video phase
transitions and stream ownership remain C interfaces.

## Audio clock ownership

Audio configuration alone does not make audio the video clock master. A
configured stream can remain silent indefinitely, so the startup gate is armed
only after valid decoded PCM has actually entered the playback FIFO. The first
queued PCM establishes one bounded wait window; later packets cannot extend it.
When playback starts, or when that wait times out, the gate is released.

The release after a timeout is sticky for that buffering period. An output-only
backlog rebase also leaves video free-running while SDL output is reanchored; it
does not reset the Opus/wire sequence or PTS timeline. A confirmed device
underflow is different: it ends the consumed playback period, clears the output
anchor, and allows the next PCM period to arm one new bounded startup wait.

The SDL postmix counter is only an estimate of device progress because callbacks
can advance through silence or other clients. The media playhead is therefore
bounded by this stream's queued PCM, and starvation is accepted only when the
submitted media range has been consumed and this stream's queue is empty.

## Decoded video geometry

Video packet headers carry both visible and coded dimensions. Codecs may require
even coded dimensions for an odd visible desktop (for example, 65x49 visible in
66x50 coded storage). YUV420P and NV12 decoder output is copied only across the
visible rectangle into a packed IYUV client buffer; stride and right/bottom
codec padding are never part of the presented frame. Unsupported or unusual
decoded layouts use the swscale fallback.

SDL presentation geometry is computed from the renderer's physical output size,
not the window's logical input coordinates. Video textures use nearest-neighbor
scaling. Exact source/output dimensions are the only `pixel_exact` case;
fractional high-DPI scaling and letterboxing use deterministic integer
destination rectangles.

## Frame storage and GPU-resident video

Captured and decoded frames have explicit storage identity. `wd_frame` owns
either CPU XRGB storage or DRM PRIME plane descriptors; copying a frame object
means retaining CPU storage or duplicating DRM descriptors, never borrowing an
implicit backend lifetime.

On the server, tile ownership continues to use the CPU XRGB framebuffer because
the tile codec needs CPU-visible damage. Video ownership is different. After a
VAAPI encoder is configured and its video-processing context is available,
wlroots output buffers are exported as DRM PRIME before the output state is
committed. Eligible XRGB/ARGB single-plane surfaces are handed through the frame
worker without a full-frame CPU snapshot. libva imports that surface and VPP
converts it directly into the encoder's NV12 VAAPI surface. If export, format,
VPP, device, or backend capability is unavailable, capture remains on the
existing CPU readback/sws/upload path.

This makes the intended server paths:

```
tiles: wlroots -> CPU XRGB -> tile encoder
video/VAAPI: wlroots -> DRM PRIME -> VAAPI VPP -> VAAPI encoder
video/software/fallback: wlroots -> CPU XRGB -> swscale -> encoder
```

The first VAAPI frame may still use CPU capture because encoder/VPP capability
is not known until configuration succeeds. Subsequent video-owned frames may use
DRM PRIME. Transitioning back to tile ownership requests a real compositor full
refresh before tile generations are published, so a GPU-resident video period
does not leave the CPU recovery framebuffer authoritative by accident.

On the client, `ClientVideoFrameBuffer` is move-only and may own either packed
IYUV bytes or a DRM PRIME frame. VAAPI decode can export an FFmpeg hardware
frame to DRM PRIME without `av_hwframe_transfer_data()`, but this is
capability-gated: the decoder only selects GPU output when the active presenter
sets `video_gpu_present_supported`. The current SDL texture presenter does not
claim that capability, so ordinary SDL presentation intentionally continues to
use the tested CPU IYUV fallback. A future EGL/Vulkan/other external-memory
presenter can enable the already-owned DRM PRIME path without changing decoder
queue semantics.

Tile presentation no longer requires the network thread to mutate the shared
framebuffer on the normal path. A completed tile moves into a bounded immutable
present queue. The render thread uploads those bytes directly to the SDL texture
and then updates its CPU recovery image. Queue overflow deliberately falls back
to the previous framebuffer/dirty-grid path rather than dropping visual state.
This removes the normal network-framebuffer lock handoff and the
framebuffer-to-staging copy while retaining a lossless backpressure fallback.

Regression coverage includes frame descriptor lifetime/clone behavior, move-only
client GPU frames, GPU capture eligibility, tile present queue coalescing and
bounds, GPU-frame ownership across the video present queue, and a source
contract that keeps the zero-copy/fallback boundaries explicit.

## Flow control

Control and input messages take priority over bulk media. Media work carries generation or sequence identity so stale work can be rejected before expensive processing and again before publication.

No subsystem may grow an unbounded queue. Telemetry samples may always be dropped rather than delaying media or input.

## Further reading

- [Protocol](protocol.md)
- [Threading contract](threading.md)
- [Security model](../SECURITY.md)


The stream controller applies health and tile/video ownership policy on the fixed health cadence; telemetry only snapshots and reports its results.

Every connection is a hard policy boundary: interval feedback, input correlation, pressure history, bootstrap state, and recovery state are reset before the new connection identity is published. Every connection begins with tile ownership and a compositor-produced full refresh. Automatic and forced video both remain blocked until the client reports presentation of that exact content epoch. A video-to-tile handoff likewise completes only after the client reports the recovery epoch, so an unrelated or stale tile presentation cannot release the transition. Planned resize recovery returns directly to the previously selected video mode—forced or automatic—after that acknowledgement; decode, publication, channel, and presentation failures retain the retry circuit breaker. Video-health classification uses both the current decode-queue depth and the maximum depth observed during the feedback interval so audio-synchronized frames are not mistaken for a stalled presentation pipeline merely because the queue drained just before telemetry was sampled. Audio-wait classification also requires an explicit client playback state. A buffering audio epoch may delay initial video for one bounded startup window; if no audio clock is established, the client relinquishes the gate, presents video, and reports the timeout so the server does not treat an indefinite hold as healthy.

Capture service timing follows the effective adaptive FPS target and compositor refresh. The millisecond Wayland timer uses a bounded target-derived service interval, while an absolute nanosecond deadline remains the authoritative capture gate. Eventfd wakeups may accelerate queue service but cannot advance a capture deadline or exceed the configured FPS cap.

The client owns active-session cadence. Its normalized requested FPS is applied
to the headless output before the server publishes that connection's
configuration, and the same value is the remote capture ceiling and local
presentation cap. `WD_SERVER_IDLE_REFRESH_HZ` provides the valid headless mode before a client connects. Display-size changes preserve the
active client cadence; a later connection may select a different cadence.

The eventfd wake path is lock-free and may be called while the network mutex is held.

The stream-frame worker owns framebuffer comparison and full-frame video snapshot copies outside the network mutex. It reacquires the mutex only to apply changed-tile generations, validate epochs and channel state, and enqueue transport work.

UDP receive-ring teardown is terminal: bounded cancellation is attempted first, then the ring is closed before the socket may be closed or reused. No receiver object survives disconnect or reconfiguration.

Incremental TCP readers maintain two deadlines: an idle-progress deadline refreshed by every successful read, and a hard total frame lifetime that never moves. Slow but progressing frames remain valid without allowing an unbounded frame to monopolize a channel.


Mode-transition diagnostics include bootstrap/recovery epochs, recovery class, wait duration, and retry cooldown. Client ownership logs include both the previous and accepted content owner/epoch so a frozen display can be correlated with the exact handoff that produced it.


## Stream lifecycle scenario contract

Dependency-light component scenarios compose the ownership policy across bootstrap, resize, recovery, and reconnect boundaries; they are not substitutes for a complete live client/server session. Separate runtime-seam tests exercise bounded control-handshake reads, production async-TCP teardown, SDL direct-tile upload/readback, and audio publication/playback where optional dependencies are available. The enforced policy remains: every connection begins tile-owned and must present the exact bootstrap content epoch before video can own the display, planned resize recovery resumes only after the exact recovery epoch is presented, recovery failures use the retry circuit breaker, and reconnects cannot consume presentation evidence from the previous session.

### Connection bandwidth plans

The throughput probe establishes a stable safe-link ceiling.  It is not the
same value as the adaptive tile sender rate: mode-local congestion control may
reduce tile traffic without lowering the next video encoder target.  From the
safe-link estimate the server builds a nominal class plan:

- video ownership: 75% video, 10% audio class, 10% control class, 5% overhead;
- tile ownership: 70% fresh tiles, 5% repair, 10% audio class, 10% control
  class, 5% overhead.

Audio reserves only its negotiated wire requirement within its 10% class cap.
The remaining class capacity is headroom unless the scheduler explicitly lends
it to media.  The 5% overhead share is never lent.  Client rate caps reduce the
safe-link ceiling before the plan is calculated.

Bandwidth enforcement is class-specific. Fresh tile traffic and repair traffic
use independent token buckets at their 70% and 5% nominal rates. Either tile
class may borrow the other's accumulated tokens only when the other class has
no queued work, making the scheduler work-conserving without losing the repair
guarantee. Control TCP traffic has its own 10% bucket and is never charged to
tile media. Entering or leaving video ownership resets all class tokens and
mode-local congestion streaks; it does not discard the safe link estimate.

Telemetry names the layers explicitly: `link_safe` is the probe/cap ceiling,
`link_recent` is the plan basis, `tile_media` is the current adaptive aggregate,
and the fresh, repair, video, audio, control, and overhead fields are the
current class allocations. Per-minute tile telemetry reports actual fresh and
repair bytes separately and reports predicted fresh demand against the fresh
allocation. These names intentionally avoid treating every connection budget
as a UDP rate, because video, audio, and control use TCP transports.

### Automatic tile/video selection

Automatic entry evaluates dirty coverage across every sampled frame, not only
frames that changed. Sustained average coverage of 30% selects video directly.
Lower-coverage workloads may also select video when estimated fresh-tile wire
demand reaches 85% of the fresh-tile allocation. The estimate combines the
observed wire cost per covered base tile, the all-frame dirty average, the
current geometry, and the client-requested frame rate; successfully transmitted
bytes are retained only as an observed-pressure signal.

While automatic video owns the display, the compositor still records cheap
damage coverage metadata. A return to tiles requires this average to remain at
or below 15% for 30 seconds. Dormant tile queues, repair backlog, and tile-budget
blocking are intentionally not exit requirements because those producers are
paused during video ownership. Forced video bypasses content thresholds only;
it still requires the client's selected control mode, successful negotiation,
a connected video channel, an encoder, completed bootstrap/recovery, and any
failure cooldown required by the recovery class.

### Planned resize continuity

A planned resize records whether video was selected before the geometry change.
The server temporarily transfers ownership to one exact tile recovery snapshot,
binds that snapshot to the current framebuffer generation, and suppresses later
live tile churn while it drains. A second resize cancels the obsolete barrier
and restarts it against the newest framebuffer generation. Once the client
presents the exact recovery epoch, the controller moves directly to
`video-ready` when video negotiation, channel, encoder, and requested mode are
still valid. Automatic selection is preserved just like forced selection; a
planned resize does not require a new dirty-content qualification window.

The client treats replacement textures as pending surfaces. The last
successfully presented surface remains visible while the new geometry is
allocated and populated. A tile replacement commits only after every base tile
of the newest recovery frame is present and the upload/presentation succeeds; a
video replacement commits on a fresh successful keyframe presentation. Stale,
partial, or failed replacements leave the previous surface active.

### In-place video recovery

The compressed decode-input queue and the decoded presentation queue are
separate bounded resources. A transient compressed-queue overflow discards the
now-undecodable dependency chain, sends immediate typed feedback, and waits for
a replacement keyframe. The server keeps video ownership, enters
`video-recovering`, requests that keyframe from the encoder, and waits for
presentation of the exact recovery frame. The last valid video texture remains
visible; no video EOS or tile ownership transition occurs for the bounded
recovery attempts.

Typed feedback distinguishes transient overload from a hard decoder or
publication failure. Overload and keyframe dependency loss recover in place.
A hard failure, closed video transport, or exhausted recovery attempts transfers
ownership to a full tile recovery. Presentation-stall and hard-failure streaks
are tracked independently so unrelated one-second samples cannot combine into a
false fallback.

### Video cadence is adaptive below the client ceiling

The client `--session-fps` request is a ceiling, not a promise that every video frame
will be encoded at that rate. Decode-input overload or average decode time that
leaves less than the configured headroom immediately lowers video capture and
encoder cadence. Cadence rises one FPS at a time only after the configured sustained-health interval.
Compositor refresh remains at the client-selected session cadence while video
capture may run below it. Tile pressure, video overload, and presentation
health use independent streaks.

Every recovery decision emits the complete causal sample: feedback flags and
sequence, receive/decode/presentation progress, compressed and presentation
queue occupancy, decoder phase, keyframe wait state, A/V hold age, audio state,
and active/requested FPS. This log is the authoritative explanation for a
video-to-tile transition; the bandwidth-plan reset that follows is a consequence
of ownership changing, not the cause.

## Video-owned framebuffer shadow

While video owns the display, the frame worker skips tile/shadow framebuffer
comparison and invalidates the shadow. Auto mode continues to sample compositor
**damage metadata** for its exit decision; it does not need a tile diff. On a
video-to-tile handoff the compositor must provide a new full-refresh frame
before the tile pipeline can publish a generation. Video-ready (before the
first keyframe) remains tile-owned and retains normal tile diffing. A mode
change racing with a skipped analysis requests another full frame instead of
mistaking the skipped analysis for an unchanged image.

## Video snapshot preflight

Before copying the CPU framebuffer into a video snapshot, the frame worker
checks whether an encoded video frame is still queued with the async TCP
sender. A backpressured frame is not copied. The final publication check under
the network lock still runs because the sender can become busy while the
snapshot is being copied. After encoding, another pending-message check and
keyframe rearm are required to protect interframe decoder references.

## Owned video TCP buffers

The video worker prepares a complete async TCP wire allocation and copies the
encoded access unit into it once. The TCP sender takes ownership on enqueue;
its normal partial-send, completion, drop, cancel, and shutdown paths release
the same allocation. A failed enqueue consumes the prepared allocation too.
Other control-message senders retain their existing copy-based API.

## Xwayland decoration work

Repeated Xwayland buffer commits no longer reapply unchanged titlebar geometry
and enabled state. First scene association, late map requests that create nodes,
fullscreen transitions, window width changes, and reassociation invalidate or
change the cached layout. Each content commit still propagates damage; a
stable decoration layout does **not** mean the game drew an unchanged picture.
`compositor-capture/interval` reports decoration layout updates and reuses.
`x11_committed_bounds_mpix` is a sum of buffer bounding areas, **not** actual
changed-pixel area or a per-Wine-process metric. Do not interpret it as a
damage-rectangle estimate or use it to suppress render/readback.
