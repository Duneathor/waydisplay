# WayDisplay

WayDisplay is an experimental low-latency remote display system for Linux. The server runs a headless wlroots compositor and streams tiles, video, audio, clipboard data, and input events to an SDL client.

Planned output resizes use an exact tile recovery frame while retaining the last visible client surface, then resume whichever video mode was previously selected. Video decode cadence adapts below the client-requested FPS ceiling without changing the compositor session cadence.

The project is not deployed and the protocol is intentionally unstable. Protocol compatibility may be broken whenever doing so improves latency, throughput, or maintainability.

## Trust model

WayDisplay currently provides **no authentication or encryption**. Run it only on localhost, a trusted private network, or through a VPN. Do not expose the server port directly to an untrusted network.

Both peers must be little-endian Linux systems. Big-endian hosts and non-Linux targets are intentionally unsupported. liburing is a required dependency for every build, and the wlroots server is pinned to the `wlroots-0.20` pkg-config ABI. The io_uring implementation is restricted to operations available in Linux 5.14; newer kernels are supported but newer-only operations are not used.

## Build

A native optimized build can be configured and compiled with:

```sh
cmake --preset native
cmake --build --preset native
```

The resulting executables are:

```text
waydisplay-server
waydisplay-client
```

See [BUILDING.md](BUILDING.md) for dependencies, profiles, feature switches, tests, and installation.

On Arch Linux, use the root-level local `PKGBUILD` to install dependencies,
build both runtime binaries, run tests, and install the package directly from
your current checkout (including uncommitted changes):

```sh
makepkg -si
```

Without an external `BUILDDIR`, package build outputs go into ignored
`src/build-*` and `pkg/` directories.
Do not pass `-C` or `-c` without setting an out-of-tree `BUILDDIR` first: makepkg's
default `src/` directory is the project's tracked source tree. See [BUILDING.md](BUILDING.md).
This local-checkout recipe is not suitable for publishing to the AUR without
replacing its empty source list with reproducible, pinned sources.

## Run

Start the server first:

```sh
./build-native/waydisplay-server --app konsole
```

`konsole` is the default app and must be installed on the server; use `--app`
to choose another command.

Then connect the client using the address and ports selected for that server:

```sh
./build-native/waydisplay-client 127.0.0.1 5000 6000
```

For a remote host, use that server's reachable IPv4 address instead of
`127.0.0.1`; bind it with `--listen` on a trusted network. The client prefers
H.265; `--video-codec h264` selects H.264 explicitly. Both `--video-encoder`
(server) and `--video-decode` (client) accept `off|auto|software|vaapi`,
defaulting to `auto`.

**Ctrl+Alt+right-click** in the client opens a launcher menu. **LAUNCH DEFAULT**
reopens the configured server app; **LAUNCH APPLICATION** prompts for a server-side
command. Only use this with a trusted server/client pair.

Use `--help` for all arguments and see [Command line](docs/command-line.md)
for modes and [HEVC troubleshooting](docs/video-hevc-troubleshooting.md) for
diagnostic traces (DEBUG builds only).

## Design priorities

The project makes tradeoffs in this order:

1. Latency
2. Throughput
3. Graphical correctness
4. Audio correctness
5. Security

Memory safety, bounded queues, parser limits, and nonblocking network progress remain mandatory because failures in those areas directly damage latency and availability.

## Documentation

- [Architecture](docs/architecture.md)
- [Protocol](docs/protocol.md)
- [Threading contract](docs/threading.md)
- [Security model](SECURITY.md)
- [Command line](docs/command-line.md)
- [HEVC troubleshooting and DEBUG trace](docs/video-hevc-troubleshooting.md)
- [Build, tests, and safe Arch packaging](BUILDING.md)
- [Open work](TODO.md)
