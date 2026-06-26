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

Capture service timing follows the effective adaptive FPS target and compositor refresh. The millisecond Wayland timer uses a bounded target-derived service interval, while pacing admits a frame within one service tick of its deadline to avoid rate halving at integer-millisecond boundaries.
