# HEVC trace checklist

When HEVC stalls but H.264 runs, collect simultaneous client/server INFO logs
with the same session. `video trace` samples frames 1–8 and then every 128th
frame **per encoder frame-ID sequence** (and only on the current client video
TCP channel). It is on by default for these sparse samples; no payload bytes
are dumped. Packet hashes are diagnostic only, not authentication.

Compare `epoch`, `frame`, `bytes`, `prefix`, and `hash` in `server-send` and
`client-recv`. An identical fingerprint does not prove all bytes match
cryptographically. A difference proves the copies are different, except in
case of a hash collision. `server-drop` means the frame was encoded but not
transmitted; the encoder is instructed to emit a keyframe next.

Then compare `client-dequeue` queue wait, `client-decode` duration and output,
`client-publish` queue depth, and `client-upload`/`client-present`. An absent
stage localizes the first transition that stopped. Frame IDs attached to
`client-decode`'s output may lag the input frame ID due to codec buffering.
`encode_ms` includes codec reconfiguration, and `queue_ms` is elapsed time
since the server worker job was published (not TCP one-way network latency).

The log is intentionally sampled and is **not** a complete per-frame transport
trace. Use `--video-codec h264` as the temporary workaround if HEVC remains
unstable. Do not increase the decode queue simply to suppress overflow; that
can increase latency instead of recovering correct reference frames.

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

For deeper diagnostics without rebuilding with CMake manually, run
`WAYDISPLAY_PACKAGE_LOG_LEVEL=DEBUG makepkg -sif` from the checkout root.
This compiles DEBUG logging into the **Release binaries** while retaining
Release optimization and Arch hardening flags. The separate Debug test tree
always uses DEBUG. A normal `makepkg -sif` reconfigures Release back to INFO.
The package release number is unchanged by this optional diagnostic switch;
use it only for a local package, not a distributed reproducible build.

## Audio clock ahead of slow video

If `client-decode` and `client-publish` succeed but every frame logs
`client-discard reason=audio-sync` at `present_depth=1`, software encoding may
be slower than the audio clock. The presenter now drops a late picture only
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
Server and client trace hashes match when this occurs: it is **encoder
output**, not TCP corruption. The server repairs only this recognizable
escaped-prefix pattern for HEVC VA-API output and never rewrites packets
that already start with valid Annex-B. This narrowly scoped workaround
is not a replacement for a complete bitstream parser. Test both IDR and
interframes on the actual driver; check that `server-send` and
`client-recv` start with `00000001` (or `000001`).

Frame IDs restart when the stream content epoch advances. Repeated frame-1
keyframes within one epoch are also possible during encoder restart; a
second IDR must replace decoder references even if frame 1 was already
presented. A later video epoch can legitimately be dropped as stale if
an EOS/tile-recovery epoch has already taken ownership.
