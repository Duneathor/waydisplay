# Building WayDisplay

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
not supported by the VA device. Select a backend explicitly with:

```sh
waydisplay_server_wlroots --video-encoder vaapi \
  --vaapi-device /dev/dri/renderD128 --app foot
waydisplay_server_wlroots --video-encoder software --app foot
```

The first VA-API implementation still converts XRGB to NV12 in system memory
and uploads that frame to a VA surface. It removes software H.264/H.265 encoding
from the hot path, but is not a zero-copy compositor-to-encoder path. Check
`vainfo` for `VAEntrypointEncSlice`; older AMD hardware may support H.264 encode
without HEVC encode, in which case use client option `--video-codec h264`.

## Tile-size selection

The server advertises its runtime tile dimensions in the `WD_MSG_SERVER_CONFIG`
payload. The default remains `128x64`; for bandwidth-limited/high-latency links,
start the wlroots server with smaller tiles, for example:

```sh
waydisplay-server --wan-tiles
# or explicitly:
waydisplay-server --tile-size 64x64
```

Smaller tiles can reduce over-send for small UI changes, at the cost of more tile
metadata and packetization overhead.

## WAN client budget

WayDisplay now uses one adaptive max-rate transport instead of full/partial/
limited/live stream modes. The server starts at the throughput-probed UDP tile
budget and adapts that byte rate and render cadence from feedback. For shared
or known-constrained links, the client can request a cap below the probe:

```sh
waydisplay-client <server> 5000 6000 --rate-kib 4096
# or a more conservative shared-link cap:
waydisplay-client <server> 5000 6000 --rate-kib 2048
# shorthand for a constrained Wi-Fi/WAN link:
waydisplay-client <server> 5000 6000 --wan
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
ctest --preset tests
```

The preset disables the optional SDL and wlroots executables so protocol,
transition, repair, and planning tests do not depend on desktop development
packages.
