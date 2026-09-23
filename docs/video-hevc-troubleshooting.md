# HEVC diagnostics and recovery

## Keep the ordinary logs actionable

Use a STATS build and run both endpoints with `-v` to see health, real frame
rates, byte counts, and resize/stream-ownership transitions. The per-frame packet-stage logger and its test helpers were removed. A DEBUG build is no longer required for routine HEVC diagnosis.

```sh
WAYDISPLAY_PACKAGE_LOG_LEVEL=STATS makepkg -sif
waydisplay-server -v
waydisplay-client 192.168.0.183 -v
```

An intentionally resized client window causes a remote display change and a
new content epoch. Wait for the new dimensions to settle before comparing
quality or throughput. Compare `video-cadence/interval`,
`client-video-cadence/interval`, `video-stream/interval`, and
`client-video/interval` rather than multiplying sampled frame-ID gaps into a
frame-rate estimate. The rate target is not a measured bitrate or a quality
metric.

## VA-API HEVC rate control and visual quality

On a link whose derived HEVC video budget reaches the existing maximum of
100,000 KiB/s, the server prefers constant QP (`CQP`, `qp=18`) rather than
leaving VA-API to choose rate control from an enormous bitrate target. On a
lower-bandwidth link, existing automatic rate control remains in effect.
Look for `HEVC VAAPI quality mode: rc=CQP qp=18` in the server's `-v` output.
If the encoder/driver rejects this configuration, WayDisplay retries ordinary
VA-API rate control rather than silently forcing software HEVC.

A small packet can represent an unchanged frame; its size alone does not prove
compression loss. Compare sharp colored text and moving video at a fixed
resolution with `--video-mode off` and `--video-mode force`; see
[video quality](video-quality.md) for how to compare aligned images. The
screenshot showing large green blocks warrants this test, but cannot isolate
a rate-control error from reference-frame corruption or a capture bug.

## Decoder recovery and packet integrity

The VA-API HEVC Annex-B escaped-prefix repair still runs for **every affected
packet**, even though its per-frame diagnostic was removed. For a malformed
access unit, the server rejects the frame and requests a keyframe; warnings
remain visible with `-v`. Encoded frames dropped after encoding due to TCP
backpressure also request a fresh keyframe: subsequent reference frames must
not depend on pictures the client never received.

A `video decoder reset: reason=video recovery keyframe` on entry to a new
content epoch is normal, not by itself evidence of a queue overflow. Actual
decode failures, queue overflow, invalid recovery keyframes, and server
`video health decision` fallback are logged independently. Enabling `-v`
remains useful when they occur, with no packet-stage spam.

For a control test, run HEVC versus H.264 or tiles at the same output dimensions
and monitor failures, packet counts, and measured client presentation FPS.
