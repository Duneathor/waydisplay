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
