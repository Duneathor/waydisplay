# Building WayDisplay

## Language baseline and branch hints

WayDisplay requires C11 for C sources and C++20 for C++ sources. Supported GCC
builds therefore need a compiler with complete support for the C++20 statement
attributes used by the client hot paths. CMake configures C++ targets with
`-std=c++20` and rejects compilers that cannot provide that language level.

Use `[[likely]]` and `[[unlikely]]` only on branches with a stable runtime bias:
steady-state packet/render success, or exceptional validation, allocation, I/O,
and shutdown paths. Do not annotate policy choices whose frequency depends on
content, link quality, or user configuration; PGO remains the preferred source
of workload-specific branch probabilities. The `waydisplay.cpp20_branch_hints`
test compiles both attributes and the build-profile contract verifies the
C++20 compiler flag in every checked-in profile.

## GCC build profiles

WayDisplay supports GCC and G++ only. The checked-in CMake presets provide four
single-purpose build profiles:

| Preset | Compiler behavior | Default log level |
| --- | --- | --- |
| `debug` | `-O0 -g3 -ggdb -fno-omit-frame-pointer` | `DEBUG` |
| `release` | `-O3` plus GCC link-time optimization | `INFO` |
| `profile` | Native `-O3`/LTO build instrumented with `-fprofile-generate` | `INFO` |
| `native` | `-O3`, LTO, `-march=native`, and PGO when data is available | `INFO` |

`WAYDISPLAY_LOG_LEVEL` is the CMake logging build option. It accepts `OFF`,
`ERROR`, `WARN`, `INFO`, `STATS`, or `DEBUG`; each level includes all levels to
its left. Periodic client and server telemetry uses `STATS`, visible with `-v`.
The per-frame packet-stage logger has been removed, including from DEBUG builds. Decode failures,
queue overflow, video health, and encoded-frame drops remain available at `WARN`
with `-v`; errors remain visible even in quiet mode.

```sh
cmake --preset native -DWAYDISPLAY_LOG_LEVEL=STATS
```

The package-specific `WAYDISPLAY_PACKAGE_LOG_LEVEL` setting is described below;
it is not a runtime flag and does not affect CMake builds directly.

Build a portable optimized binary with:

```sh
cmake --preset release
cmake --build --preset release
```

Use `Native` for a binary intended only for the machine on which it is built:

```sh
cmake --preset native
cmake --build --preset native
```

Without profile data, `Native` still uses `-O3`, GCC LTO, `-march=native`, and
`-mtune=native`. CMake reports `pgo_data_available=FALSE` during configuration.

### GCC profile-guided optimization workflow

`Profile` and `Native` intentionally share `build-native`. This keeps GCC's
object-derived profile names stable between profile generation and profile use.
Profile data is stored in `build-pgo-data`; it is a CMake cache path, not an
environment variable or runtime setting.

Start by clearing stale profile data and building the instrumented binaries:

```sh
cmake --preset profile
cmake --build build-native --target pgo-clean
cmake --build --preset profile --clean-first
```

Run a representative WayDisplay workload with the instrumented client and
server. Exercise the codecs, tile/video transitions, input, audio, clipboard,
reconnect, and shutdown paths that matter for the intended deployment. Exit the
processes normally so GCC flushes their counters.

Then reconfigure the same build tree for profile use and rebuild it:

```sh
cmake --preset native
cmake --build --preset native --clean-first
```

The Native configure summary must report `pgo_data_available=TRUE`. If no
`.gcda` files exist, CMake deliberately omits `-fprofile-use` and produces the
non-PGO native build instead. GCC coverage-mismatch diagnostics remain enabled;
profile data from changed sources must be discarded and regenerated rather than
silently accepted.

Direct configurations may select the same profiles with
`-DCMAKE_BUILD_TYPE=Debug|Release|Profile|Native`. Override
`WAYDISPLAY_PGO_DATA_DIR` only when the generation and use configurations point
to the same persistent directory.

For a clean dependency-light compile check of the common library, disable the SDL
client and wlroots server targets:

```sh
cmake -S . -B build-common \
  -DWAYDISPLAY_BUILD_CLIENT_SDL=OFF \
  -DWAYDISPLAY_BUILD_WLROOTS_SERVER=OFF
cmake --build build-common
```

On Arch Linux, the repository-root `PKGBUILD` builds the current checkout and
runs the full-runtime test suite before installing the Release executables:

```sh
makepkg -si
```

**Clean-build safety:** the repository's tracked `src/` directory is also
makepkg's default `$srcdir` when the PKGBUILD is at the root. Do **not** use
`makepkg -C` or `makepkg -c` with the default build directory: those options
remove `$srcdir`, which would remove source files. The PKGBUILD refuses those
options when it detects the collision, but a separate build directory is the
safe way to use clean builds. Set `BUILDDIR` to an absolute directory outside
this checkout in `~/.makepkg.conf`, for example:

```sh
BUILDDIR="$HOME/.cache/makepkg"
```

With that configured, `makepkg -sifCc` from the repository root is safe, and
ordinary `makepkg -si` continues to work. If you do not configure `BUILDDIR`,
use `makepkg -sif` without `-C`/`-c`. Do not use `git clean` as a substitute
for makepkg's clean-build operation.

To install Release binaries with detailed media traces while preserving Release
optimization and Arch hardening, build **both** endpoints with:

```sh
WAYDISPLAY_PACKAGE_LOG_LEVEL=DEBUG makepkg -sif
```

Normal `makepkg -sif` switches Release back to `INFO` and removes routine
per-frame traces. At runtime, both executables are quiet by default
(errors only); pass `-v` or `--verbose` **on each endpoint** to enable the
levels present in that build. A STATS or DEBUG package therefore still needs
`-v` to show its stats or sampled trace messages. The separate Debug test
build always uses `DEBUG`; it does not change installed Release binaries.
See [HEVC troubleshooting](docs/video-hevc-troubleshooting.md) for interpreting
matching server/client frame hashes, stages, and recovery warnings.

The full server build requires the `wlroots0.20` package, providing the
`wlroots-0.20` pkg-config module (version 0.20.0 or newer). The 0.19 ABI is not
an accepted fallback. After changing wlroots versions, rebuild from clean
CMake caches so imported headers and libraries cannot be mixed.

A full build needs SDL3 with Vulkan support for the client and wlroots/Wayland
development packages for the compositor server.

## Network exposure
To listen on a specific IPv4 interface, pass `--listen-ipv4`:

```sh
# Local machine only:
waydisplay-server --listen-ipv4 127.0.0.1 --tcp-port 5000 --launch-command konsole

# All IPv4 interfaces; use only on a trusted network:
waydisplay-server --listen-ipv4 0.0.0.0 --tcp-port 5000 --launch-command konsole
```

`--listen-ipv4` accepts an IPv4 address, not a hostname or an address-and-port pair.
The connection token associates WayDisplay transport channels; it is not remote
user authentication or transport encryption.

## Optional video mode

The video-mode path can use FFmpeg/libavcodec for H.264, H.265, and AV1 when the codec
libraries are available. The build still succeeds without them; in that case the
encoder/decoder backends report `none` and video negotiation remains disabled.

To enable the real video path, install FFmpeg development packages that provide
pkg-config files for `libavcodec`, `libavutil`, and `libswscale`, then build with:

```sh
cmake -S . -B build \
  -DWAYDISPLAY_ENABLE_H264_SERVER_ENCODER=ON \
  -DWAYDISPLAY_ENABLE_H264_CLIENT_DECODER=ON \
  -DWAYDISPLAY_ENABLE_H265_SERVER_ENCODER=ON \
  -DWAYDISPLAY_ENABLE_H265_CLIENT_DECODER=ON \
  -DWAYDISPLAY_ENABLE_AV1_SERVER_ENCODER=ON \
  -DWAYDISPLAY_ENABLE_AV1_CLIENT_DECODER=ON
cmake --build build
```

The client defaults to H.265. Use `--video-codec auto` to advertise both H.265
and H.264, or `--video-codec h264`,
`--video-codec h265`, or `--video-codec av1` to select a specific codec.
AV1 is opt-in, not part of `--video-codec auto`. In automatic server mode,
codec negotiation prefers a codec supported by
the VA-API device before falling back to software encoding.

The server defaults to `--video-encoder auto`, which tries FFmpeg's VA-API
encoder first and falls back to `libx264`/`libx265`/`libaom-av1` when the selected codec is
not supported by an automatically discovered VA device. Select a backend explicitly with:

```sh
waydisplay-server --video-encoder vaapi --launch-command konsole
waydisplay-server --video-encoder software --launch-command konsole
waydisplay-server --video-encoder off --launch-command konsole  # tiles only
```

The client uses `--video-decoder <off|auto|software|vaapi>` (default `auto`).
`off` does not advertise the encoded-video channel; `software` decodes video
without attempting VA-API. `--video-mode off` is the coarse video-policy switch.


The first VA-API implementation still converts XRGB to NV12 in system memory
and uploads that frame to a VA surface. It removes software H.264/H.265/AV1 encoding
from the hot path, but is not a zero-copy compositor-to-encoder path. Check
`vainfo` for `VAEntrypointEncSlice`; older AMD hardware may support H.264 encode
without HEVC encode, in which case use client option `--video-codec h264`.
HEVC VA-API output is normalized to Annex-B when a recognized escaped start-code
prefix occurs. The repair is deliberately narrow, and does not guarantee that
all vendor-specific bitstream defects are accepted.

## Tile-size selection

Tile geometry is build-time stream policy in `include/waydisplay/wd_config.h`.
`WD_TILE_WIDTH` and `WD_TILE_HEIGHT` define the base grid advertised in
`WD_MSG_SERVER_CONFIG`. The stream encoder may aggregate dirty base tiles into
the configured 128x64, 64x64, 32x32, and 16x16 wire-tile ladder according to
packet, compression, and link-budget conditions.

Changing the base geometry affects tile IDs, queue sizing, and damage behavior;
rebuild and run the full test suite rather than selecting a per-launch tile mode.

## WAN client budget

WayDisplay now uses one adaptive max-rate transport instead of full/partial/
limited/live stream modes. The server starts at the throughput-probed UDP tile
budget and adapts that byte rate and render cadence from feedback. For shared
or known-constrained links, the client can request a cap below the probe:

```sh
waydisplay-client <server> --link-cap-kib-per-sec 4096
# or a more conservative shared-link cap:
waydisplay-client <server> --link-cap-kib-per-sec 2048
```

The requested budget is a cap: the server will not raise its throughput-probed
safe ceiling to satisfy it. This is useful when the link is shared or when the
startup probe overestimates sustainable long-haul throughput. On a clean Wi-Fi link, start with 2048-4096 KiB/s and raise it after checking client completion and retransmit telemetry.

## Client tile texture uploads

The SDL client coalesces incoming tile rectangles, then chooses the cheapest of
three streaming-texture upload plans: one lock per coalesced rectangle, one
fully initialized bounding-box lock, or one full-frame lock. SDL texture locks
are write-only, so bounding/full locks always copy every pixel in the locked
region from the client framebuffer.

The cost model treats one texture lock as roughly 128K copied pixels. Runtime
telemetry exposes `texture_locks`, `bounds_uploads`, `cost_full`, `source_mpix`,
and `upload_mpix` in `client-render/interval:`. Compare `source_mpix` with
`upload_mpix` to see the extra copy area accepted to reduce lock calls, and
compare `texture_locks` with `remote_frames` to verify that fragmented tile
updates are usually reduced to one lock per presented frame.

## Render geometry limit

WayDisplay protocol zero limits the negotiated render surface to **4096x2160**.
The tile protocol uses 16-bit base-tile IDs and counts with a fixed 16x16 base
grid; enforcing this 4K-class limit prevents grid-count truncation and keeps
framebuffer allocation bounded. Both client-requested sizes and server config
updates are rejected when they exceed this limit.

## Relocatable unit-test workflow

Do not copy or invoke a generated `CTestTestfile.cmake` from another checkout;
CMake build trees intentionally contain absolute paths. From any source-tree
location, regenerate the test build with the checked-in presets:

```sh
cmake --preset tests
cmake --build --preset tests
```

The default build includes the `run_tests` target, so a test failure also fails
the build. Run the same target explicitly at any time with:

```sh
cmake --build build-tests --target run_tests
```

For an iterative build that compiles tests without running them on every build,
configure with `-DWAYDISPLAY_RUN_TESTS_ON_BUILD=OFF`; the explicit `run_tests`
target remains available. `ctest --preset tests` can also be used to rerun the
suite directly.

To build and run only the video control policy regression test through the
existing CTest architecture (without starting a server/client or building the
entire test suite), use:

```sh
cmake --preset tests-core -DWAYDISPLAY_RUN_TESTS_ON_BUILD=OFF
cmake --build build-tests-core --target waydisplay_test_video_control_unit
ctest --preset tests-core -R '^waydisplay\.video_control_unit$' --no-tests=error
```

This still uses the project's normal CMake configure dependencies (including
liburing), but compiles and executes only the video control test target.

New tests should be registered with the `waydisplay_add_test()` helper in
`CMakeLists.txt`. The helper creates the executable, registers it with CTest,
and adds it to the build-time test dependencies, so no separate executable list
needs to be maintained.

Library-focused tests should link only the library under test unless another
dependency is part of the test itself. This keeps transitive link contracts
covered: for example, `waydisplay.client_runtime_linkage` verifies that the
client runtime supplies its common logging and thread dependencies to consumers.

The `tests` preset disables the optional SDL and wlroots executables so
protocol, transition, repair, and planning tests do not depend on desktop
development packages.

Additional presets isolate the optional build contracts that have historically
been easy to miss when only the dependency-light suite is compiled:

```sh
# No SDL, wlroots, audio, FFmpeg, or VAAPI code paths.
cmake --preset tests-core
cmake --build --preset tests-core

# Real software codec, resize round-trip, and VAAPI tests when FFmpeg is installed.
# This preset treats warnings as errors so optional-only test sources receive the
# same warning coverage as the dependency-light strict build.
cmake --preset tests-codecs
cmake --build --preset tests-codecs

# Real wlroots scene tests and generated protocol headers.
cmake --preset tests-wlroots
cmake --build --preset tests-wlroots

# Recompile the same wlroots tests against the non-Xwayland struct layout.
cmake --preset tests-wlroots-no-xwayland
cmake --build --preset tests-wlroots-no-xwayland

# Build every optional client/server backend available on the machine.
cmake --preset tests-full
cmake --build --preset tests-full
```

The wlroots presets deliberately disable audio and video codecs so a generated
Wayland-header or scene-test failure is not hidden by an unrelated hardware
codec test. The full preset restores all default optional backends.

### Focused audio/video regression slices

The synchronization and visible-frame regressions are split so most failures can
be reproduced without opening an audio device or invoking a codec. After a
`tests-core` configure, the dependency-light cases can be selected with:

```sh
ctest --test-dir build-tests-core --output-on-failure   -R 'waydisplay\.(audio_startup_state|audio_playback_clock|audio_video_sync|audio_startup_integration|video_plane_copy|video_decoder_conversion|video_presentation_geometry)$'
```

`audio_startup_state` models only startup-gate transitions; it does not require
SDL or Opus. `audio_playback_clock` covers queue rebasing, postmix/playhead
accounting, and starvation confirmation. The video plane/conversion/geometry
tests similarly operate on deterministic in-memory fixtures.

The real encoder/decoder roundtrip remains in the codec preset because it
requires FFmpeg and at least one codec shared by the software encoder and
decoder:

```sh
cmake --preset tests-codecs -DWAYDISPLAY_RUN_TESTS_ON_BUILD=OFF
cmake --build build-tests-codecs --target waydisplay_test_video_codec_roundtrip
ctest --test-dir build-tests-codecs --output-on-failure   -R '^waydisplay\.video_codec_roundtrip$' --no-tests=error
```

That roundtrip deliberately uses odd visible dimensions backed by even coded
dimensions, associates decoded image content with frame ID/PTS, and repeats the
check after a resize/reconfigure. A skipped codec roundtrip is not equivalent to
passing the dependency-light conversion tests; both layers are useful when
debugging visual corruption.

## Runtime arguments and build-time policy

The supported client/server command lines and the options intentionally kept in
`wd_config.h` are documented in [`docs/command-line.md`](docs/command-line.md).
Legacy aliases are rejected rather than silently translated.

`include/waydisplay/wd_config.h` is the single build-time policy surface. It
owns defaults, thresholds, timeouts, queue and cache ceilings, retry counts,
codec quality/latency choices, estimator coefficients, and product visual
settings. The `waydisplay.config_tunable_contracts` test prevents representative
policy constants from drifting back into implementation files.

Wire-format sizes and masks remain in `wd_protocol.h`; time-unit conversions
remain in `wd_time.h`; backend ABI values, keycodes/modifier bits, and codec
hard limits remain beside the API that defines them. Those values are
correctness or compatibility invariants, not product tunables.

Protocol assumptions and the little-endian-only wire contract are documented in `docs/protocol.md`.

## Installation

After configuring and building a runtime profile, install the available executables and documentation with:

```sh
cmake --install build-native --prefix "$HOME/.local"
```

The installed executable names are `waydisplay-server` and `waydisplay-client`. A target is installed only when its required dependencies were found and the target was built.

## Continuous integration profiles

The repository provides dedicated presets for strict warnings and sanitizers:

```sh
cmake --preset tests-warnings
cmake --build --preset tests-warnings
ctest --preset tests-warnings

cmake --preset tests-asan-ubsan
cmake --build --preset tests-asan-ubsan
ctest --preset tests-asan-ubsan

cmake --preset tests-tsan
cmake --build --preset tests-tsan
ctest --preset tests-tsan
```

The package-heavy SDL, wlroots, codec, VAAPI, and audio matrix is kept separate from required dependency-light checks.


## Coverage and fuzzing

Run the dependency-light coverage configuration with:

```sh
cmake --preset tests-coverage
cmake --build --preset tests-coverage
ctest --preset tests-coverage
gcovr --root . --filter 'src/' --print-summary
```

Coverage-guided tile protocol and reassembly fuzzers are available with Clang by configuring `-DWAYDISPLAY_BUILD_FUZZERS=ON`. Fuzz binaries are not registered as ordinary CTest cases; CI or local fuzz jobs should provide a corpus and run duration explicitly. Every CTest receives a tier label (`unit`, `integration`, `stress`, `fuzz`, or `hardware`) in addition to subsystem labels. The `tests-full` preset requires both runtime executable targets so missing SDL3 or wlroots dependencies cannot silently reduce coverage.


### Video scrub and recovery tests

`waydisplay.video_feedback_protocol`, `waydisplay.video_inplace_recovery`,
`waydisplay.video_adaptive_cadence`, and `waydisplay.video_scrub_recovery`
cover the production feedback descriptor, transient-overload recovery,
cause-specific health streaks, bounded keyframe retries, and client-FPS ceiling.
The scrub test intentionally fills the compressed decode-input queue and proves
that overload remains in video ownership, while hard decode/publication flags
select tile fallback. Keep these tests in the dependency-light suite so recovery
policy changes do not require FFmpeg or SDL hardware.

`waydisplay.planned_resize_resume`, `waydisplay.render_planning`, and
`waydisplay.resize_video_continuity` cover the complete planned-resize contract:
rapid resizes supersede obsolete framebuffer-generation barriers, selected
forced or automatic video resumes after the exact tile epoch, partial or stale
replacement textures cannot hide the last valid frame, and the decode-rate
controller settles near its measured safe ceiling without the former
multiplicative sawtooth.

### Bandwidth and cadence policy tests

`waydisplay.bandwidth_plan`, `waydisplay.server_tile_policy`, and
`waydisplay.frame_pacing` are dependency-light policy tests. They verify that
video and tile class percentages fit the safe-link budget, automatic video
selection uses all-frame turnover and predicted fresh-tile demand, forced mode
still obeys negotiated control readiness, and client cadence normalization is
shared by compositor and stream pacing. Keep these tests enabled in every core
profile when changing transport allocation or frame timing.

### AV1 video (opt-in)

Use **matching new client and server binaries**; older protocol-zero builds do not
recognize AV1's codec capability bit. The server probes `av1_vaapi` on the
selected GPU; `--video-encoder software` requires FFmpeg's `libaom-av1` encoder.
AV1 VA-API decoding uses FFmpeg's native `av1` hardware decoder. Software
AV1 decoding uses FFmpeg's `libdav1d` decoder (or `libaom-av1` when dav1d is
unavailable). The native `av1` decoder is hardware-only: choosing a software
pixel format on it does not enable software decoding. Check `ffmpeg -decoders`
for `libdav1d` or `libaom-av1` if AV1 software decoding is unavailable.

```sh
waydisplay-server --video-encoder vaapi --launch-command konsole
waydisplay-client 192.168.0.183 --video-codec av1 --video-decoder auto
```

For a software encoder test, replace the server's `vaapi` with `software`;
software AV1 can be substantially slower than hardware. Software AV1 uses
libaom realtime `cpu-used=8` (the FFmpeg wrapper's highest supported speed)
with four encoder threads, zero lookahead, and row-based multithreading.
At desktop resolutions it now selects `2x1` or `2x2` AV1 tiles to expose
more parallel work; smaller images use `1x1`. More tiles can increase bitrate
at equal visual quality, and this is **not** a guarantee of 60 fps.
Compare `video-stream/interval` `frame_attempts` and `encode_total_ms` before/after,
and check `client-video/interval` decoded/presented plus audio underflows. The
first keyframe may still be substantially slower than subsequent frames.
After four non-keyframe software-AV1 samples, video-active capture pacing
is also capped using the encoder's measured moving-average frame time with
15% headroom. This avoids repeatedly rendering and copying 60 snapshots/s
when the encoder is handling around 13. **It does not change negotiated FPS,
encoder configuration, stream timestamps, decoder cadence, or the tiles path.**
The cap resets on tiles/video bandwidth-mode transitions, and ignores slow
startup and periodic keyframes. `state` STATS reports the actual
`capture_pacing_fps`; compare it with `video-stream/interval` `frame_attempts`,
`frames_tx`, `encode_total_ms`, and superseded frames before/after.
AV1 VA-API encoding
and AV1 Profile 0 VA-API decoding are separate device capabilities: a server
may encode AV1 on its GPU even when the client's GPU cannot decode it. With
`--video-decoder auto`, WayDisplay checks the selected libva device and uses
software AV1 decoding using libdav1d or libaom-av1 when AV1 Profile 0 VLD
is unavailable. This requires a software AV1 decoder in the installed FFmpeg
build; the native `av1` hardware decoder cannot be used for that fallback.
Explicit `--video-decoder software` selects the software decoder directly.
`--video-decoder vaapi` requires device support; the hardware decoder test skips
unsupported AV1 rather than reporting a false codec regression. If the GPU does not
support AV1 encode, `--video-encoder vaapi` cannot negotiate AV1 and will
remain in tiles mode. Run `--video-codec h265` to retain the established HEVC
path. AV1 sends raw length-delimited OBUs, not Annex-B NAL units; HEVC's VA-API
start-code workaround must never process AV1 packets.

### Xwayland window geometry

Managed fullscreen X11 windows use the full logical output height without WayDisplay's titlebar. Maximized windows retain the titlebar; valid small X11 utility windows retain their requested sizes. The `waydisplay.xwayland_geometry` unit test covers this policy.

### Xwayland lifecycle

X11 map requests only configure a window; WayDisplay exposes its scene after the associated Wayland surface actually maps. Minimized X11 scene nodes are hidden; a later map or unminimize restores them. X11 transient parent references resolve only through live view entries, and child references are detached before parent destruction. Fullscreen/maximized managed windows ignore conflicting stale client configure geometry.

### Compositor and Xwayland profiling

`WAYDISPLAY_PACKAGE_LOG_LEVEL=STATS makepkg -sif` compiles in one
`compositor-capture/interval` aggregate (run the server with `-v` to see it) alongside `server-loop/interval`,
`video-stream/interval`, and `client-video/interval`. Only the `x11_` fields describe
mapped Xwayland root surfaces, whether or not the application is Wine.
`x11_committed_bounds_mpix` sums full committed buffer bounds: it is **not**
unique changed pixels. The `scene_build_*`, `texture_read_*`, and
`buffer_data_fallbacks` fields describe the entire composed output, including
native Wayland surfaces; `texture_read_*` counts only
`wlr_texture_read_pixels` attempts, not CPU buffer-data fallbacks.
`server-loop/interval render_readback_avg_ms` includes additional output work:
these timings overlap and must not be added. Neither measures Wine/DXVK render
time or isolates GPU time. INFO compiles out the extra per-frame timing.

### Naming and log field conventions

[The naming guide](docs/naming.md) distinguishes Xwayland/X11 activity from
whole-compositor capture, documents the units and semantics of the new capture
fields and the canonical CLI and metric schema.
Run `waydisplay.compositor_capture` for the updated aggregate helper test.
