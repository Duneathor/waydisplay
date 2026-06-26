# Security model

## Current status

WayDisplay does not currently authenticate peers and does not encrypt traffic. The connection token correlates channels within a session; it is not a password, identity proof, or cryptographic secret.

The server accepts remote input, clipboard data, media transport, and application-launch-related state. Exposing it to an untrusted network can therefore give an untrusted peer significant control over the session.

## Supported deployment

Run WayDisplay only in one of these environments:

- localhost
- a trusted private network
- a trusted VPN that provides peer authentication and encryption

Do not forward the WayDisplay ports directly from the public internet.

## In-scope defensive behavior

Even on trusted networks, the implementation must retain:

- strict frame and payload bounds
- nonblocking incremental network reads
- partial-frame deadlines
- bounded queues and allocation sizes
- complete protocol validation before dispatch
- memory-safe cleanup and idempotent shutdown
- rejection of stale session, generation, and sequence data

These protect availability, latency, and memory safety in addition to security.

## Platform assumptions

Only little-endian Linux peers are supported. Big-endian systems are rejected at compile time. Protocol version `0` has no backward-compatibility commitment.

## Future authentication

A later design may add a token or password. That work should define credential storage, handshake behavior, channel binding, failure reporting, and migration together rather than treating the current connection token as authentication.

## Reporting issues

Until a dedicated private reporting channel exists, avoid publishing secrets, private network addresses, or exploit payloads in public issue text. Provide a minimal reproduction with sensitive data removed.
