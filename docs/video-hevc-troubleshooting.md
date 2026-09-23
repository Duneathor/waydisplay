# HEVC diagnostics and recovery

## Enable a diagnostic build

A normal `makepkg -sif` produces optimized Release binaries at `INFO`.
Sampled `video trace stage=…` lines on **both** the server and client require
`DEBUG` at compile time. Install the same log-level build on both machines:

```sh
WAYDISPLAY_PACKAGE_LOG_LEVEL=DEBUG makepkg -sif
```

DEBUG keeps WayDisplay's sampled video diagnostics but sets wlroots itself to
`WLR_INFO`, avoiding its per-surface/FBO debug chatter. The server's routine
VA-API HEVC escaped-prefix repair runs on **every affected packet**; its
`server-hevc-repair` message is sampled alongside the other video trace stages
(frames 1–8 and each 128th). The packet fix is never sampled or skipped.

For a CMake build, configure with `-DWAYDISPLAY_LOG_LEVEL=DEBUG` and rebuild.
There is no runtime switch for enabling compiled-out logging. To return to the
quiet optimized package, use `makepkg -sif` without the override. The Debug
test tree always uses `DEBUG` independently of the packaged Release binaries.
Keep the `PKGBUILD`'s safe out-of-tree `BUILDDIR` setting if using makepkg's
`-C`/`-c` cleanup; see [BUILDING.md](../BUILDING.md).

## Read the trace

When HEVC stalls but H.264 works, collect simultaneous client/server DEBUG
logs from the same connection. The trace samples frames 1–8 and every 128th
frame per encoder frame-ID sequence. It is **not** a complete per-frame
transport trace; no full payload bytes or connection token are logged. Packet
hashes are diagnostics, not cryptographic authentication or wire-integrity
checks.

Compare `epoch`, `frame`, `bytes`, `prefix`, and `hash` in `server-send` and
`client-recv`. A matching fingerprint is evidence for the same packet bytes,
not proof against collisions. A sampled encoded frame dropped because a
previous video TCP send is still pending is reported at WARN as `video encoded
frame dropped: … reason=pending-tcp rearm=keyframe`. Even when the warning
falls outside the sampling window, the encoder requests a fresh keyframe rather
than continuing with a dependent frame.

Then compare `client-dequeue` queue wait, `client-decode` duration and output,
`client-publish` depth, and `client-upload`/`client-present`. An absent stage
localizes the transition that stopped. Output frame IDs may lag input IDs due
to codec buffering. `encode_ms` includes codec reconfiguration; `queue_ms`
measures elapsed time since the server job was published, **not** network
one-way latency. Normal decode failures, queue-overflow warnings, and server
health fallbacks remain visible without DEBUG.

Use `--video-codec h264` for a working alternative when the HEVC path remains
unhealthy. Enlarging the decode queue without fixing reference-frame recovery
may only increase latency.

## Distinguish discard from slow decoder initialization

`client-dequeue` occurs **before** content-epoch / transition checks and decoder
configuration, so it does not prove a packet reached FFmpeg. Check for the
following sampled stages for the *same* `epoch` and `frame`:

- `client-handle` then `client-drop`: the message was rejected before decoding;
  the `reason` identifies the guard. No payload or connection token is logged.
- `client-epoch`: time spent accepting the content-epoch/ownership transition;
  a missing line after `client-handle` indicates this stage is blocked.
- `client-configure-start` / `client-configure-end`: decoder setup time and
  success. A long interval here can fill the four-packet input queue even if
  subsequent `client-decode` calls take only milliseconds.
- `client-decode`, `client-publish`, `client-upload`, `client-present`: the
  original stages remain unchanged. Compare the server's matching
  `server-send` lines to avoid confusing an intentional repeat keyframe with
  a lost frame.

On overflow, `oldest` and `oldest_wait_ms` are captured *before* the input
queue is flushed. These identify the oldest **queued** packet, not the packet
already being processed by the decode worker. To distinguish those, pair the
overflow with the most recent `client-dequeue`/`client-configure-start` line.

## Audio clock ahead of slow video

If `client-decode` and `client-publish` succeed but no frame reaches
`client-present`, inspect any `client-discard reason=audio-sync` lines and
`client-sync-late` deltas. Slow encoding can put video behind the audio clock. The presenter now drops a late picture only
when another decoded picture is already queued; the newest available picture
must be shown even when late. `client-sync-late` logs sampled frame ID, video
PTS, audio playhead samples and the signed audio-video delta (milliseconds).

This avoids the false no-presentation health fallback, but it does not make a
slow software HEVC encoder real time. Compare `server-send encode_ms` with the
requested frame interval and use H.264 or hardware encoding where appropriate.
An encoded-input queue overflow *without* audio remains a separate problem;
this presentation fix does not change the input queue capacity or decoder
recovery policy.

A `client-dequeue ... reset=1` on the first accepted recovery keyframe is a
*normal decoder reset*. Its log reason is `video recovery keyframe`. A real
input-queue overflow is independently reported by the warning `video decode
queue overflow: ... action=flush-and-request-keyframe` (with depth and the
oldest queued frame). Do not infer an overflow merely from a reset line.

### VA-API HEVC escaped NAL start codes

Some VA-API HEVC drivers return dependent access units beginning
`00 00 03 00 01` instead of the required Annex-B `00 00 00 01`.
If a matching malformed prefix appears in both server and client traces,
the format problem originates before transmission, not in TCP. The server repairs only this recognizable
escaped-prefix pattern for HEVC VA-API output and never rewrites packets
that already start with valid Annex-B. This narrowly scoped workaround
is not a replacement for a complete bitstream parser. With DEBUG enabled,
check **both keyframes and interframes** for `server-send` / `client-recv`
prefixes `00000001` (or `000001`) and successful decode/presentation. The
observed hardware run remained in one epoch beyond frame 1,400; this is not a
guarantee for other drivers or device configurations.

Frame IDs restart when the stream content epoch advances for a real new
encoder session. The first video keyframe moves ownership from the tiles epoch
to the video epoch without reconfiguring the existing encoder or restarting
frame IDs. Repeated frame-1 keyframes within one epoch can still occur after
an actual encoder restart; a
second IDR must replace decoder references even if frame 1 was already
presented. A later video epoch can legitimately be dropped as stale if
an EOS/tile-recovery epoch has already taken ownership.

AV1 is a separate opt-in codec (`--video-codec av1`). Its encoded packets are
OBUs and must not be checked for Annex-B start codes or passed through the
VA-API HEVC prefix repair. Use DEBUG traces for AV1 as for HEVC, but interpret
`prefix` as raw AV1 OBU header bytes. `--video-codec auto` retains H.264/HEVC
for existing protocol-zero peers.
