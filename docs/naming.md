# Naming and telemetry conventions

WayDisplay's naming should describe **scope, action, and unit** rather than the
application used to investigate a feature. This is an unreleased project:
there are no deprecated CLI, telemetry, or C identifier aliases.

## Component and application names

| Name | Meaning | Do not confuse with |
|---|---|---|
| **WayDisplay** | Whole project; `waydisplay-server` and `waydisplay-client` are binaries. | A Wayland/X11 window. |
| **Compositor** | Server's wlroots scene and output; may contain native Wayland **and** Xwayland surfaces. | Xwayland alone, or a particular application. |
| **Xwayland** | Wayland-hosted X server providing X11 compatibility. Source filenames and test names use `xwayland`; short metric prefixes use `x11_`. | Wine. |
| **Wine** | One possible X11 application ecosystem used for exercising Xwayland. | A renderer, codec, compositor stage, or performance-counter owner. |
| **XDG** | Native Wayland XDG shell/toplevel/popup protocol paths. | X11 managed/transient/override-redirect windows. |
| **Tile / video** | Alternative ownership of the *remote image transport*. | A window type or compositor renderer. |
| **AV1 / H.264 / H.265** | Encoded bitstream codecs. CLI uses `av1`, `h264`, and `h265`; **HEVC** is the descriptive equivalent of H.265 in FFmpeg and its troubleshooting guide. | Encoder backend (`vaapi`, `software`, `libaom-av1`, etc.). |

Do not make an Xwayland fix or test Wine-only by its name unless it actually
filters Wine processes or specifically checks Wine behavior. The Xwayland
geometry and lifecycle tests are generic X11 policy tests; real Wine/DXVK
integration and throughput still need separate runtime measurements.

## Named rates and timing

- `--session-fps`, `requested_session_fps`, and `compositor_refresh_hz` refer to a
  client's requested ceiling and its active headless-output cadence; actual
  encoded frame throughput is not guaranteed by either value.
- `capture_pacing_fps` is the **effective server capture gate**, potentially
  reduced by link, decode, or software-encode throughput. It is not client
  decoder speed. `video_decode_safe_fps` is the decode-derived bound, not the
  actual presented or encoded FPS.
- `video-stream/interval encode_total_ms` is the **sum** of encoding time in a reporting
  interval. For mean encode duration, divide by `frame_attempts` when nonzero;
  `server-loop/interval encode_avg_ms` is a separately reported mean. Neither is FPS.
- `keyframe_attempts` counts input jobs that *request* keyframes;
  `keyframes_tx` counts encoded packets with the keyframe flag successfully
  queued for sending. They need not match, including when the encoder emits a
  keyframe without a new request. `frames_tx` means queued for TCP, not proof
  of client presentation; check the client presentation feedback separately.
- The `/interval` suffix means **one stats reporting window**, typically about
  one minute, not a count normalized to exactly 60 seconds. Do not treat counts
  as rates without the elapsed interval.

## Logging conventions

Everyday commands require only `waydisplay-server` and
`waydisplay-client <server_ipv4>`. TCP `5000` and local UDP `6000` are the
client defaults; `-v` / `--verbose` opts in to compiled-in diagnostic
levels independently on each endpoint. Errors remain visible by default.
The build-time log level is a ceiling, not a runtime verbosity switch.


The preferred label for a new periodic diagnostic is `owner-stage/interval:`, e.g.
`compositor-capture/interval:` or `client-video/interval:`. Prefix only where it
clarifies ownership; server-side logs already identify their process.
Client and server use the same hyphenated label grammar; no deprecated labels are emitted.

Every metric should identify its origin and have an explicit unit or documented
meaning: `_ns` for stored nanoseconds, `_ms` for logged milliseconds, `_mpix`
for **millions of pixel positions** (not megapixels of distinct damage),
`_kib` for KiB over the interval, `_kib_per_sec` for KiB/s, and `_fps`
for frames per second. An `_avg_ms` is an arithmetic mean over the named calls;
`_total_ms` is a duration sum. Neither is a percentile.

## `compositor-capture/interval`

The group is named for its **shared compositing/capture pipeline**. Its fields
have two distinct scopes:

| Field | Scope and counting point |
|---|---|
| `x11_surface_commits` | Mapped Xwayland root-surface commits (Wine **or any X11 client**). Not native Wayland commits. |
| `x11_committed_bounds_mpix` | Sum of the mapped Xwayland root buffer `width × height` at those commits, divided by 1,000,000. **Not** unique changed pixels, damage area, or output readback area. |
| `x11_maps`, `x11_unmaps`, `x11_configures` | Xwayland view lifecycle events; not native Wayland or Wine-specific counts. |
| `scene_build_calls`, `scene_build_avg_ms` | Entire output's `wlr_scene_output_build_state` attempts and mean call time (includes failed/idle calls). |
| `texture_read_calls`, `texture_read_area_mpix`, `texture_read_avg_ms`, `texture_read_failures` | `wlr_texture_read_pixels` region **attempts**, attempted area and mean call time, including failed attempts. Includes native Wayland and Xwayland; the area can overlap on separate attempts. |
| `buffer_data_fallbacks` | Successful CPU buffer-data readback when `wlr_texture_from_buffer` could not produce a texture; separate from `texture_read_*`. |

These are grouped together for correlation, **not** attribution of compositor
readback time to X11. No field identifies Wine processes, estimates DXVK render
time, measures unique damage, or isolates GPU time.
`server-loop/interval render_readback_avg_ms` includes additional work and
shares timing with the scene-build/read stages; do not add them together.

The source names now reflect the same ownership:
`wd_compositor_capture.h`, `struct wd_compositor_capture_stats`,
`wd_compositor_capture_x11_surface_commit`,
`wd_compositor_capture_scene_build`, and
`wd_compositor_capture_texture_read`. The regression is
`waydisplay.compositor_capture` and its source is
`tests/test_compositor_capture.c`.

## Command-line and wire vocabulary

| Canonical term | Meaning |
|---|---|
| `--session-fps` / `requested_session_fps` | Requested frame-cadence ceiling for a session; also determines headless-output refresh and local present cap. |
| `adaptive_capture_fps` | Capture ceiling lowered by tile/link feedback. |
| `capture_pacing_fps` | Effective capture scheduling gate, also bounded by software-encoder and decode capacity. |
| `--display-size` | Logical remote output geometry; distinct from X11/Wayland window geometry. |
| `--link-cap-kib-per-sec` / `link_cap_kib_per_second` | Cap on the safe **whole-link** budget, not a UDP socket throttle. |
| `--video-mode` | Automatic/off/forced transport ownership policy. |
| `--video-encoder` / `--video-decoder` | Server encode and client decode backend selection; distinct from `--video-codec`. |
| `--listen-ipv4` / `--tcp-port` | Server IPv4 listener address and its control TCP port. |
| `--launch-command` | Server-side shell command used for startup and the launch-default menu. |
| `--output-scale` / `--compositor-renderer` | wlroots output scale and renderer selection. |

Protocol field names may change without preserving aliases or wire compatibility.
Both endpoints must run the same development revision. Numeric codec values,
message layout, and behavior are changed only when they have a concrete reason;
a spelling cleanup by itself need not alter bitstream semantics.
