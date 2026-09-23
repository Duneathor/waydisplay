# Video quality and cadence checks

Use a STATS build and `-v` on both endpoints for interval health and cadence.
There is no per-frame stage trace.
An encoded frame count or configured target bitrate is not a fidelity score.

Capture at a fixed remote resolution for comparisons. An intentional SDL window
resize requests a new remote display size, resets the video epoch, and is a
**test boundary**, not automatically a failure. Record the new dimensions and
only compare frames produced after the resize has settled.

See the additional measurement and A/B instructions introduced in 047.

## 047: Measured cadence and image comparison

Use a STATS or DEBUG package and `-v`. `video-cadence/interval` on the
server reports **measured**, elapsed-time-normalized readbacks, snapshot
publication, encode attempts, queued TCP frames, feedback-reported decoded and
presented frames, and wire Mbit/s. `client-video-cadence/interval` independently
reports received/decoded/presented fps. Readbacks include tiles; the
configured target FPS and bitrate are *not* measured rates or quality scores.
Client feedback arrives periodically, so server-side client rates may lag the
client's local interval. Neither set measures pixels' visual accuracy.

Compare aligned screenshots using:

```sh
# Source screenshot, optionally exact-tile screenshot, and video screenshot
# must depict the SAME content at the SAME remote resolution and scale.
# Convert source images to binary RGB PPM without resizing (requires ffmpeg):
ffmpeg -i source.png -frames:v 1 -pix_fmt rgb24 source.ppm
ffmpeg -i tiles.png  -frames:v 1 -pix_fmt rgb24 tiles.ppm
ffmpeg -i video.png  -frames:v 1 -pix_fmt rgb24 video.ppm
python3 tools/compare_video_frames.py --reference source.ppm --tiles tiles.ppm --video video.ppm
```

The comparator rejects dimension mismatches and reports Y/Cb/Cr-channel PSNR,
maximum channel error and the percentage of changed RGB pixels. A `null` PSNR
means exact agreement for that channel. **This is a measurement of supplied
images, not proof of codec-only loss**: a local window screenshot can include
SDL scaling, color management and a later content frame. To measure codec-only
quality, export the encoder source and matching decoder output before SDL
scaling; WayDisplay does not yet provide that capture automatically.

For a visual A/B on localhost, keep the remote resolution fixed and run
`--video-mode off` (lossless tiles) versus `--video-mode force` (HEVC).
If you resize deliberately, treat the reset as a boundary and start a fresh
comparison after both endpoints agree on the new dimensions. Do not compare
frames across a resize or a tile/video ownership transition.

The client now logs a single `display resize requested: source=sdl-window`
line **after debounce**, not every raw SDL resize event. Compare it with the
server's applied output size and the client's config-applied acknowledgment.

## 048: Two bounded presentation/cadence fixes

048 introduced a bounded tolerance for the second snapshot timestamp gate.
051 removes that redundant gate instead: the compositor already paces accepted
readbacks. Video snapshot preflight and publication still check connectivity,
encoder readiness, sender backpressure, and current stream ownership. Actual
throughput remains subject to content, encoder, transport, and client capacity.

The client's IYUV video texture now uses `SDL_SCALEMODE_NEAREST` like its tile
texture, avoiding additional bilinear softness when a remote desktop is shown
at a different local size. This cannot restore details lost in NV12/4:2:0
encoding or bitrate control, and at fractional scales it may look pixelated.
Compare at exact 1:1 first; intentionally resizing the window changes both
the remote geometry and the experiment's reference frame.

## High-bandwidth HEVC VA-API quality mode (049)

At the existing maximum derived video budget of 100,000 KiB/s, HEVC VA-API now
prefers constant quantization (`rc_mode=CQP`, `qp=18`) over unspecified automatic
rate control with an enormous bitrate target. This is a *quality setting*, not a
promise to consume 100,000 KiB/s: simple or unchanged pictures can stay tiny.
At lower budgets, and for H.264/AV1, existing rate control is unchanged. If
high-quality VA-API configuration is unsupported, the server retries the
ordinary VA-API configuration, then uses its preexisting fallback policy.

The screenshot that motivated the change showed very large compression-like
blocks. We cannot determine from a screenshot alone whether that came from
quantization, stale references, source capture, or display scaling. Compare
identical content at a settled resolution in tile and HEVC modes. In `-v`, look
for `HEVC VAAPI quality mode: rc=CQP qp=18` and compare the same scene before/
after. If blocks remain even with CQP, investigate image/reference correctness
rather than raising the link budget again.

## 050: Snapshot admission diagnostics

The video preflight now counts `considered`, `preflight_accepted`,
`unavailable`, `not_due`, and `pending_send` while video is selected in 050;
051 removes the redundant `not_due` gate and counter.
The preflight outcomes are mutually exclusive; `publish_pending_send` are later recheck failures and are not part of
that preflight sum. These are STATS counters, not per-frame logging.

## 051: One compositor-owned capture clock

A readback already passed `wd_frame_pacing_due()`. Rechecking another
wall-clock interval in the stream worker rejected about every other 60 Hz
frame in the observed localhost run. The snapshot path now uses that
compositor decision, retaining the locked final checks for video ownership,
async-TCP backpressure, dimensions, and queue publication. When the compositor
slows because the scene is idle, snapshot rate may still drop legitimately.

## 052: Reading the new admission summary

`video-admission/interval` reports `considered` only for captured frames
examined while the stream is in video-ready, video-active or recovering
state. Preflight outcomes (`preflight_accepted`, `preflight_unavailable`,
`preflight_pending_send`) sum to `considered`. `publish_pending_send` is a
separate race-safe recheck and may prevent some accepted frames from being
published. `considered_fps` is a better denominator for video admission than
`readbacks_fps`, which includes tile-mode readbacks during a mixed interval.
An idle scene may simply produce fewer considered frames. At continuous 60 Hz
with no backpressure, expected admission is ~100%; packet and client stages
are still measured separately by `video-cadence/interval`.

## 053: Presentation replacements versus decode overload

Compressed decoder input-queue drops and decode failures retain their existing
recovery behavior. A *decoded* picture replaced in the client's presentation
queue is different: a transient replacement can be a normal freshness tradeoff
and cannot, by itself, establish an overloaded codec. The video capture-rate
controller only treats presentation replacements as pressure when there are at
least four and they approach 5% of the reporting interval's presented pictures.
The counters remain visible. Sustained losses can still reduce capture pacing.

## 054: Stable encoder clock during adaptive capture pacing

The configured encoder FPS/GOP is the requested session FPS, not the adaptive
capture cap. Encoder reconfiguration and its associated new keyframes should
therefore not occur *only* because the capture controller moves between e.g.
60, 57 and 51 fps. The compositor continues to pace actual captures using the
adaptive cap, and packet presentation timestamps remain based on the media
clock. Session/codec/size/bitrate/epoch changes may still reconfigure the
encoder; changing the capture cap alone does not.

## 055: Why rates vary

`video-cadence/interval` distinguishes requested session FPS, stable encoder
nominal FPS, adaptive capture target, and actual compositor pacing. It reports
`decode_input_drops` separately from `present_queue_replaced`, plus capture
rate up/down events. A transient replacement of an already-decoded picture
is not a codec-decode failure; repeated replacements can still signal display
backlog. Readback FPS also falls naturally when the scene is idle or the
compositor has no fresh damage, even if all configured caps remain 60. An
interval that crosses tile/video handoff or resizing is not steady-state
video performance. Compare `video-admission/interval` with the client cadence
and video-frame transmit counters before attributing a variation to HEVC.
