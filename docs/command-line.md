# Command-line and configuration policy

WayDisplay keeps the command line intentionally small. A command-line option is exposed only when it describes the current launch, the current connection, or a hardware compatibility choice. Adaptive thresholds, queue sizes, codec tuning, tile policy, and compositor feature policy live in `include/waydisplay/wd_config.h` so there is one reproducible build-time configuration.

Unrecognized options fail parsing. The command line has no aliases or compatibility
mode; the client and server must be built from the same protocol revision.

## Client command line

```text
waydisplay-client <server_ipv4> [tcp_port [client_udp_port]] [options]
```

### Options

| Argument | Purpose | Why it remains runtime-selectable |
|---|---|---|
| `<server_ipv4>` | Server address | Connection-specific. |
| `[tcp_port]` | Optional server control port; default `5000`. | Deployment-specific. |
| `[client_udp_port]` | Optional local UDP receive port; default `6000`. To specify it positionally, supply the TCP port too. | Host/network-specific. |
| `-v`, `--verbose` | Enable routine WayDisplay diagnostics; default output shows errors only. | Troubleshooting; see logging below. |
| `--session-fps <N>` | Requested session frame cadence | Becomes the compositor refresh, remote capture ceiling, and client presentation cap for the connection. |
| `--display-size <WxH>` | Requested remote output size | Session-specific. |
| `--link-cap-kib-per-sec <N>` | Upper bound for the safe connection budget | Caps the link estimate before video, tile, audio, control, and overhead allocations are calculated. |
| `--no-vsync` | Disable SDL present-vsync | Local renderer troubleshooting and latency testing. |
| `--no-audio` | Disable audio negotiation/playback | Local capability and session preference. |
| `--video-mode <auto|off|force>` | Coarse video-stream policy (also `--video-mode off` disables encoded video on the client) | `force` bypasses automatic content thresholds, but not initial bootstrap, active recovery, or failure backoff. A successfully presented planned resize recovery may return directly to forced video. |
| `--video-codec <auto|h264|h265|av1>` | Acceptable video codecs | `auto` offers H.264/H.265; AV1 is explicit to avoid unexpectedly selecting a slow software AV1 encoder. |
| `--video-decoder <off|auto|software|vaapi>` | `off` disables video negotiation; `auto` uses VA-API when available and falls back to software; `software` never requests VA-API; `vaapi` requires VA-API. Default `auto`. | Hardware/driver compatibility. |
| `--help`, `-h` | Print usage | Standard interface. |

### Logging and practical defaults

`waydisplay-server` alone launches `konsole`, listens on `0.0.0.0:5000`,
uses automatic video encoding and begins at the configured virtual output size.
`waydisplay-client 192.168.0.183` connects to TCP `5000` and binds local UDP
`6000`. Explicit positional ports remain supported; other client options may
appear after the server address or after supplied ports.

Both executables emit WayDisplay errors by default, but suppress routine
warnings, INFO, STATS and DEBUG output unless `-v` / `--verbose` is present on
that executable. `-v` enables **only the levels compiled into the installed
binary**: an INFO build cannot emit DEBUG traces. Rebuild with
`WAYDISPLAY_PACKAGE_LOG_LEVEL=STATS` or `DEBUG` as needed, then pass `-v`
on both endpoints to see those diagnostics. Command-line errors and `--help`
remain visible without `-v`. Third-party tools such as Xwayland/xkbcomp can
still write directly to stderr; this flag controls WayDisplay and the
configured wlroots/FFmpeg log thresholds, not arbitrary child-process output.

### Configuration-only

These policies are not command-line options:

| Policy | Configuration owner |
|---|---|
| Video target bitrate | `WD_VIDEO_DEFAULT_BITRATE_KIB_PER_SECOND` and derived-link budget policy. |
| Video entry dirty threshold | `WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT`. |
| Video entry duration | `WD_VIDEO_ENTER_SECONDS_DEFAULT`. |
| Video exit dirty threshold | `WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT`. |
| Video exit duration | `WD_VIDEO_EXIT_SECONDS_DEFAULT`. |

## Server command line

```text
waydisplay-server [options]
```

### Options

| Argument | Purpose | Why it remains runtime-selectable |
|---|---|---|
| `--listen-ipv4 <IPv4>` | Bind address; default `0.0.0.0`. | Deployment and exposure policy: use the default only on a trusted network or restrict the interface. |
| `--tcp-port <N>` | Control/listener port; default `5000`. | Deployment-specific. |
| `-v`, `--verbose` | Enable routine WayDisplay diagnostics; default output shows errors only. | Troubleshooting; see logging below. |
| `--launch-command <command>` | Startup application; also the Ctrl+Alt+right-click menu's **Launch default** target. Default `konsole`. | Launch-specific. |
| `--display-size <WxH>` | Virtual output dimensions | Session-specific. |
| `--output-scale <N>` | Virtual output scale | Session/display-specific. |
| `--compositor-renderer <auto|gles2|vulkan|pixman>` | wlroots renderer selection | Hardware/driver compatibility. |
| `--video-encoder <off|auto|software|vaapi>` | `off` disables encoded-video negotiation (tiles remain available); otherwise select automatic, software-only, or VA-API-only encoding. Default `auto`. | Hardware/driver compatibility. |
| `--help`, `-h` | Print usage | Standard interface. |

### Frame cadence ownership

`WD_SERVER_IDLE_REFRESH_HZ` in `wd_config.h` initializes the headless output
before the first connection and is restored after disconnect. During handshake,
the client's normalized `--session-fps` value becomes the output refresh, capture
ceiling, and client presentation cap. A later connection may select a different
rate; the compositor applies it before publishing that connection's
configuration. Live display-size requests preserve the active client-selected
cadence. There is no server-side refresh-rate option: change
`WD_SERVER_IDLE_REFRESH_HZ` only when the pre-connection product default itself
needs to change.

### Video cadence below the session ceiling

`--session-fps` is a ceiling, not a guaranteed encoded-video frame rate. The server
may adapt video capture/encode cadence below it while retaining the requested
compositor and SDL presentation cadence. See [Frame cadence ownership](#frame-cadence-ownership).

### Configuration-only

| Policy | Configuration owner |
|---|---|
| Tile size | `WD_TILE_WIDTH`, `WD_TILE_HEIGHT`, and the supported wire-tile ladder. |
| Tile compression | `WD_SERVER_TILE_COMPRESSION_BENCHMARK_MODE_DEFAULT` and compression-advisor policy. |
| Xwayland enablement | `WD_SERVER_DEFAULT_ENABLE_XWAYLAND`. |
| XDG dialog support | `WD_SERVER_DEFAULT_ENABLE_XDG_DIALOG`. |

## Changing configuration

Configuration values are compile-time product policy. Change `include/waydisplay/wd_config.h`, rebuild both endpoints when the setting affects protocol negotiation, and run the test suite. Names include units where applicable:

- `_NS`, `_US`, `_MS`, `_SECONDS`
- `_BYTES`, `_KIB`
- `_PACKETS`, `_ENTRIES`, `_SAMPLES`
- `_HZ`, `_PERCENT`, `_PX`

The static assertions at the end of `wd_config.h` reject invalid relationships such as unordered bounds, impossible tile geometry, and undersized queues.

The centralized policy includes:

- connection deadlines, link estimators, retry limits, and feedback cadence;
- async rings, decoder queues, tile-reassembly caches, and readback batches;
- stream thresholds, bandwidth shares, capture pacing, and recovery limits;
- Opus/FFmpeg latency and quality choices;
- renderer cost-model defaults and built-in client/Xwayland decoration colors.

Not every numeric constant is a tunable. Wire sizes, protocol masks, keycodes,
modifier bits, backend API constants, codec-mandated hard limits, and unit
conversion factors stay in their owning protocol or implementation headers.
Changing those values changes the wire format or algorithmic correctness,
rather than the deployment policy.

`waydisplay.config_tunable_contracts` checks representative owners and rejects
reintroduction of local policy literals. Add a new build-time knob to
`wd_config.h`, give it an explicit unit suffix where applicable, add a static
assertion for important relationships, and extend that contract when the knob
protects an architectural boundary.

### Bandwidth allocation

`--link-cap-kib-per-sec` caps the safe link estimate rather than directly setting a UDP
socket rate.  The server derives separate video, fresh-tile, repair, audio,
control, and overhead allocations from that capped estimate.  Tile adaptation
changes only the current tile-media rate; it does not lower the stable link
ceiling used when video ownership begins.

Automatic video defaults to a 30% all-frame dirty-coverage entry threshold and
a 15% exit threshold held for 30 seconds. The entry controller can select video
below 30% when predicted fresh-tile demand reaches 85% of the current fresh-tile
allocation. `--video-mode force` bypasses these content thresholds but not protocol,
channel, bootstrap, recovery, or encoder readiness checks.

### Emergency application launcher

While the SDL client is connected, **Ctrl+Alt+right-click** opens the local
context menu. **LAUNCH DEFAULT** starts the server's `--launch-command` again (default
`konsole`), even when every remote window has been closed. **LAUNCH APPLICATION**
opens a command prompt; type a command and press Enter, or Escape to cancel.
The command runs **on the server** in the compositor's Wayland environment
under the server user account, through `/bin/sh -c`. It is not client-local
execution. The launcher is session-bound but has no authentication boundary:
see [Security](../SECURITY.md) and [Protocol](protocol.md#application-launch-requests).
