# Building WayDisplay

## GCC build profiles

WayDisplay supports GCC and G++ only. The checked-in CMake presets provide four
single-purpose build profiles:

| Preset | Compiler behavior | Debug logging |
| --- | --- | --- |
| `debug` | `-O0 -g3 -ggdb -fno-omit-frame-pointer` | Enabled |
| `release` | `-O3` plus GCC link-time optimization | Disabled |
| `profile` | Native `-O3`/LTO build instrumented with `-fprofile-generate` | Disabled |
| `native` | `-O3`, LTO, `-march=native`, and PGO when data is available | Disabled |

Normal informational logging remains controlled by `WAYDISPLAY_ENABLE_LOGGING`.
The `Debug` profile always enables both normal and debug-level logging. Release,
Profile, and Native always compile out debug-level logging.

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

A full build needs SDL3 with Vulkan support for the client and wlroots/Wayland
development packages for the compositor server.

## Network exposure
To listen on a specific IPv4 interface, pass `--listen`:

```sh
# Local machine only:
waydisplay_server_wlroots --listen 127.0.0.1 --port 5000 --app foot

# All IPv4 interfaces; use only on a trusted network:
waydisplay_server_wlroots --listen 0.0.0.0 --port 5000 --app foot
```

`--listen` accepts an IPv4 address, not a hostname or an address-and-port pair.
The connection token associates WayDisplay transport channels; it is not remote
user authentication or transport encryption.

## Optional video mode

The video-mode path can use FFmpeg/libavcodec for H.264 and H.265 when the codec
libraries are available. The build still succeeds without them; in that case the
encoder/decoder backends report `none` and video negotiation remains disabled.

To enable the real video path, install FFmpeg development packages that provide
pkg-config files for `libavcodec`, `libavutil`, and `libswscale`, then build with:

```sh
cmake -S . -B build \
  -DWAYDISPLAY_ENABLE_H264_SERVER_ENCODER=ON \
  -DWAYDISPLAY_ENABLE_H264_CLIENT_DECODER=ON \
  -DWAYDISPLAY_ENABLE_H265_SERVER_ENCODER=ON \
  -DWAYDISPLAY_ENABLE_H265_CLIENT_DECODER=ON
cmake --build build
```

The client defaults to H.265. Use `--video-codec auto` to advertise both H.265
and H.264, or `--video-codec h264` / `--video-codec h265` to force a specific
codec. In automatic server mode, codec negotiation prefers a codec supported by
the VA-API device before falling back to software encoding.

The server defaults to `--video-encoder auto`, which tries FFmpeg's VA-API
encoder first and falls back to `libx264`/`libx265` when the selected codec is
not supported by an automatically discovered VA device. Select a backend explicitly with:

```sh
waydisplay_server_wlroots --video-encoder vaapi --app foot
waydisplay_server_wlroots --video-encoder software --app foot
```

The first VA-API implementation still converts XRGB to NV12 in system memory
and uploads that frame to a VA surface. It removes software H.264/H.265 encoding
from the hot path, but is not a zero-copy compositor-to-encoder path. Check
`vainfo` for `VAEntrypointEncSlice`; older AMD hardware may support H.264 encode
without HEVC encode, in which case use client option `--video-codec h264`.

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
waydisplay-client <server> 5000 6000 --rate-kib 4096
# or a more conservative shared-link cap:
waydisplay-client <server> 5000 6000 --rate-kib 2048
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
and `upload_mpix` in `[client render/min]`. Compare `source_mpix` with
`upload_mpix` to see the extra copy area accepted to reduce lock calls, and
compare `texture_locks` with `remote_frames` to verify that fragmented tile
updates are usually reduced to one lock per presented frame.

## Render geometry limit

WayDisplay protocol v37 limits the negotiated render surface to **4096x2160**.
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

New tests should be registered with the `waydisplay_add_test()` helper in
`CMakeLists.txt`. The helper creates the executable, registers it with CTest,
and adds it to the build-time test dependencies, so no separate executable list
needs to be maintained.

The `tests` preset disables the optional SDL and wlroots executables so
protocol, transition, repair, and planning tests do not depend on desktop
development packages.

Additional presets isolate the optional build contracts that have historically
been easy to miss when only the dependency-light suite is compiled:

```sh
# No SDL, wlroots, audio, FFmpeg, or VAAPI code paths.
cmake --preset tests-core
cmake --build --preset tests-core

# Real software codec, round-trip, and VAAPI tests when FFmpeg is installed.
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

## Runtime arguments and build-time policy

The supported client/server command lines and the options intentionally kept in
`wd_config.h` are documented in [`docs/command-line.md`](docs/command-line.md).
Legacy aliases are rejected rather than silently translated.
