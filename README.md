# WayDisplay

WayDisplay is an experimental low-latency remote display system for Linux. The server runs a headless wlroots compositor and streams tiles, video, audio, clipboard data, and input events to an SDL client.

The project is not deployed and the protocol is intentionally unstable. Protocol compatibility may be broken whenever doing so improves latency, throughput, or maintainability.

## Trust model

WayDisplay currently provides **no authentication or encryption**. Run it only on localhost, a trusted private network, or through a VPN. Do not expose the server port directly to an untrusted network.

Both peers must be little-endian Linux systems. Big-endian hosts and non-Linux targets are intentionally unsupported. liburing is a required dependency for every build, and the wlroots server is pinned to the `wlroots-0.19` pkg-config ABI. The io_uring implementation is restricted to operations available in Linux 5.14; newer kernels are supported but newer-only operations are not used.

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

See [BUILDING.md](BUILDING.md) for dependencies, profiles, feature switches, testing, sanitizers, installation, and troubleshooting.

## Run

Start the server first:

```sh
./build-native/waydisplay-server
```

Then connect the client using the address and ports selected for that server:

```sh
./build-native/waydisplay-client 127.0.0.1 5000 6000
```

Use `--help` on either executable for the complete command-line interface. See [docs/command-line.md](docs/command-line.md) for examples.

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
