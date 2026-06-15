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

## Optional H.265 video mode

The video-mode scaffolding can use FFmpeg/libavcodec for H.265 when the codec
libraries are available. The build still succeeds without them; in that case the
encoder/decoder backends report `none` and video negotiation remains disabled.

To enable the real H.265 path, install FFmpeg development packages that provide
pkg-config files for `libavcodec`, `libavutil`, and `libswscale`, then build with:

```sh
cmake -S . -B build \
  -DWAYDISPLAY_ENABLE_H265_SERVER_ENCODER=ON \
  -DWAYDISPLAY_ENABLE_H265_CLIENT_DECODER=ON
cmake --build build
```

The server prefers the `libx265` encoder when FFmpeg exposes it, falling back to
FFmpeg's generic HEVC encoder lookup. The client uses FFmpeg's HEVC decoder and
converts decoded frames back to XRGB8888 for the existing SDL texture presenter.

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
