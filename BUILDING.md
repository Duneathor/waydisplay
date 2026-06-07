# Building WayDisplay

For a clean dependency-light compile check of the common library, disable the SDL
client and wlroots server targets:

```sh
cmake -S . -B build-common \
  -DWAYDISPLAY_BUILD_CLIENT_SDL=OFF \
  -DWAYDISPLAY_BUILD_WLROOTS_SERVER=OFF
cmake --build build-common
```

A full build needs SDL2 for the client and wlroots/Wayland development packages
for the compositor server.

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

For bandwidth-limited links, the client can request a limited-mode UDP tile
budget below the server's throughput-probe estimate:

```sh
waydisplay-client <server> 5000 6000 --mode limited --limited-rate-kib 4096
# or a more conservative shared-link cap:
waydisplay-client <server> 5000 6000 --mode limited --limited-rate-kib 2048
# shorthand for very constrained links:
waydisplay-client <server> 5000 6000 --wan
```

The requested budget is a cap: the server will not raise its throughput-probed
safe ceiling to satisfy it. This is useful when the link is shared or when the
startup probe overestimates sustainable long-haul throughput. On a clean Wi-Fi link, start with 2048-4096 KiB/s and raise it after checking client completion and retransmit telemetry.
