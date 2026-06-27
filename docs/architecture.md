# Architecture

## System shape

WayDisplay has two processes:

- `waydisplay-server` runs a headless wlroots compositor, captures damage, chooses tile or video transport, captures audio, and accepts client input.
- `waydisplay-client` receives media and state, performs decode and reassembly, presents through SDL, and sends local input and clipboard updates.

The protocol is version `0`. There are no compatibility guarantees while the software remains undeployed.

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

Wire structures are encoded explicitly. The implementation requires little-endian Linux hosts but does not transmit compiler padding or native ABI layouts. liburing is mandatory, and the compositor targets the wlroots 0.19 ABI explicitly. The transport probes `send`, `sendmsg`, `recv`, and `async_cancel` at ring creation and intentionally avoids operations introduced after Linux 5.14.

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

Transport/session ownership is centralized. Protocol handling, render planning, reassembly, synchronization, and telemetry are kept independent from SDL where practical. Audio/video timing, video phase transitions, and stream ownership are implemented in C; new non-SDL logic should also prefer C.

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

The test suite exercises complete ownership scenarios rather than only isolated transition predicates. Every connection begins tile-owned and must present the exact bootstrap content epoch before video can own the display, including forced-video sessions. Planned resize recovery resumes previously selected forced or automatic video immediately after the exact recovery epoch is presented; decoder, channel, or presentation failures use a retry circuit breaker. Reconnects rotate the connection identity and cannot consume presentation evidence from the previous session.

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
frames that changed. Sustained average coverage of 50% selects video directly.
Lower-coverage workloads may also select video when estimated fresh-tile wire
demand reaches 85% of the fresh-tile allocation. The estimate combines the
observed wire cost per covered base tile, the all-frame dirty average, the
current geometry, and the client-requested frame rate; successfully transmitted
bytes are retained only as an observed-pressure signal.

While automatic video owns the display, the compositor still records cheap
damage coverage metadata. A return to tiles requires this average to remain at
or below 20% for 30 seconds. Dormant tile queues, repair backlog, and tile-budget
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

The client `--fps` request is a ceiling, not a promise that every video frame
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
