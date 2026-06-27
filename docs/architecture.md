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

Every connection is a hard policy boundary: interval feedback, input correlation, pressure history, bootstrap state, and recovery state are reset before the new connection identity is published. Every connection begins with tile ownership and a compositor-produced full refresh. Automatic and forced video both remain blocked until the client reports presentation of that exact content epoch. A video-to-tile handoff likewise completes only after the client reports the recovery epoch, so an unrelated or stale tile presentation cannot release the transition. Planned resize recovery may return directly to forced video after that acknowledgement; decode, publication, channel, and presentation failures retain the retry circuit breaker. Video-health classification uses both the current decode-queue depth and the maximum depth observed during the feedback interval so audio-synchronized frames are not mistaken for a stalled presentation pipeline merely because the queue drained just before telemetry was sampled. Audio-wait classification also requires an explicit client playback state. A buffering audio epoch may delay initial video for one bounded startup window; if no audio clock is established, the client relinquishes the gate, presents video, and reports the timeout so the server does not treat an indefinite hold as healthy.

Capture service timing follows the effective adaptive FPS target and compositor refresh. The millisecond Wayland timer uses a bounded target-derived service interval, while an absolute nanosecond deadline remains the authoritative capture gate. Eventfd wakeups may accelerate queue service but cannot advance a capture deadline or exceed the configured FPS cap.

The eventfd wake path is lock-free and may be called while the network mutex is held.

The stream-frame worker owns framebuffer comparison and full-frame video snapshot copies outside the network mutex. It reacquires the mutex only to apply changed-tile generations, validate epochs and channel state, and enqueue transport work.

UDP receive-ring teardown is terminal: bounded cancellation is attempted first, then the ring is closed before the socket may be closed or reused. No receiver object survives disconnect or reconfiguration.

Incremental TCP readers maintain two deadlines: an idle-progress deadline refreshed by every successful read, and a hard total frame lifetime that never moves. Slow but progressing frames remain valid without allowing an unbounded frame to monopolize a channel.


Mode-transition diagnostics include bootstrap/recovery epochs, recovery class, wait duration, and retry cooldown. Client ownership logs include both the previous and accepted content owner/epoch so a frozen display can be correlated with the exact handoff that produced it.


## Stream lifecycle scenario contract

The test suite exercises complete ownership scenarios rather than only isolated transition predicates. Every connection begins tile-owned and must present the exact bootstrap content epoch before video can own the display, including forced-video sessions. Planned resize recovery may resume forced video immediately after the exact recovery epoch is presented; decoder, channel, or presentation failures use a retry circuit breaker. Reconnects rotate the connection identity and cannot consume presentation evidence from the previous session.
