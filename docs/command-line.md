# Command-line and configuration policy

WayDisplay keeps the command line intentionally small. A command-line option is retained only when it describes the current launch, the current connection, or a hardware compatibility choice. Adaptive thresholds, queue sizes, codec tuning, tile policy, and compositor feature policy live in `include/waydisplay/wd_config.h` so there is one reproducible build-time configuration.

Removed options are rejected as unknown arguments. They are not retained as deprecated aliases.

## Client command line

```text
waydisplay-client <server_ipv4> <tcp_port> <client_udp_port> [options]
```

### Retained

| Argument | Purpose | Why it remains runtime-selectable |
|---|---|---|
| `<server_ipv4>` | Server address | Connection-specific. |
| `<tcp_port>` | Server control port | Deployment-specific. |
| `<client_udp_port>` | Local UDP receive port | Host/network-specific. |
| `--fps <N>` | Requested capture/presentation cap | Useful per display and connection. |
| `--size <WxH>` | Requested remote output size | Session-specific. |
| `--rate-kib <N>` | Upper bound for adaptive tile traffic | Connection-specific bandwidth cap. |
| `--no-vsync` | Disable SDL present-vsync | Local renderer troubleshooting and latency testing. |
| `--no-audio` | Disable audio negotiation/playback | Local capability and session preference. |
| `--video <auto|off|force>` | Coarse video-stream policy | `force` bypasses automatic content thresholds, but not initial bootstrap, active recovery, or failure backoff. A successfully presented planned resize recovery may return directly to forced video. |
| `--video-codec <auto|h264|h265>` | Acceptable video codecs | Hardware/driver compatibility. |
| `--video-hwdecode <off|auto|vaapi>` | Decoder backend policy | Hardware/driver compatibility. |
| `--help`, `-h` | Print usage | Standard interface. |

### Configuration-only

These are no longer command-line options:

| Former argument | Configuration owner |
|---|---|
| `--video-bitrate-kib` | `WD_VIDEO_DEFAULT_BITRATE_KIB_PER_SECOND` and derived-link budget policy. |
| `--video-min-dirty-percent` | `WD_VIDEO_MIN_DIRTY_PERCENT_DEFAULT`. |
| `--video-enter-seconds` | `WD_VIDEO_ENTER_SECONDS_DEFAULT`. |
| `--video-exit-dirty-percent` | `WD_VIDEO_EXIT_DIRTY_PERCENT_DEFAULT`. |
| `--video-exit-seconds` | `WD_VIDEO_EXIT_SECONDS_DEFAULT`. |

### Removed legacy aliases

- `--limited-rate-kib`: use `--rate-kib`.
- `--wan`: use an explicit `--rate-kib <N>` when a cap is needed.
- `--mode`: obsolete adaptive-streaming predecessor.

## Server command line

```text
waydisplay-server [options]
```

### Retained

| Argument | Purpose | Why it remains runtime-selectable |
|---|---|---|
| `--listen <IPv4>` | Bind address | Deployment and exposure policy. Remote clients normally require `--listen 0.0.0.0` or a specific interface address. |
| `--port <N>` | Control/listener port | Deployment-specific. |
| `--app <command>` | Application launched inside the compositor | Launch-specific. |
| `--size <WxH>` | Virtual output dimensions | Session-specific. |
| `--scale <N>` | Virtual output scale | Session/display-specific. |
| `--refresh-hz <N>` | Virtual output refresh rate | Session/display-specific. |
| `--renderer <auto|gles2|vulkan|pixman>` | wlroots renderer selection | Hardware/driver compatibility. |
| `--video-encoder <auto|software|vaapi>` | Encoder backend selection | Hardware/driver compatibility. |
| `--help`, `-h` | Print usage | Standard interface. |

### Configuration-only

| Former argument | Configuration owner |
|---|---|
| `--tile-size` | `WD_TILE_WIDTH`, `WD_TILE_HEIGHT`, and the supported wire-tile ladder. |
| `--tile-compression` | `WD_SERVER_TILE_COMPRESSION_BENCHMARK_MODE_DEFAULT` and compression-advisor policy. |
| `--xwayland`, `--no-xwayland` | `WD_SERVER_DEFAULT_ENABLE_XWAYLAND`. |
| `--xdg-dialog`, `--no-xdg-dialog` | `WD_SERVER_DEFAULT_ENABLE_XDG_DIALOG`. |

### Removed legacy aliases

- `--wan-tiles`: tile selection is adaptive; configure the base tile geometry directly when developing a different policy.

## Changing configuration

Configuration values are compile-time product policy. Change `include/waydisplay/wd_config.h`, rebuild both endpoints when the setting affects protocol negotiation, and run the test suite. Names include units where applicable:

- `_NS`, `_US`, `_MS`, `_SECONDS`
- `_BYTES`, `_KIB`
- `_PACKETS`, `_ENTRIES`, `_SAMPLES`
- `_HZ`, `_PERCENT`, `_PX`

The static assertions at the end of `wd_config.h` reject invalid relationships such as unordered bounds, impossible tile geometry, and undersized queues.
