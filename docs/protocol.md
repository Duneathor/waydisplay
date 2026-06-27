# WayDisplay protocol

WayDisplay currently uses protocol version 0. The protocol may change without backward compatibility while the project remains undeployed.

## Platform contract

Both peers must run on little-endian Linux hosts. Big-endian and non-Linux systems are intentionally unsupported; configuration fails on such targets. Protocol zero sends packed, fixed-width C structures directly. GCC-compatible one-byte packing, little-endian field order, and the compile-time asserted structure sizes are the current wire ABI. Compatibility with other compilers, architectures, or future protocol revisions is not promised. Variable media and selection tails remain opaque byte sequences.

The expected deployment is localhost, a trusted network, or a VPN. Protocol version 0 does not provide authentication or encryption.

## Presentation acknowledgements

`WD_MSG_CLIENT_STATS` carries the latest tile and video content epochs actually presented by SDL. These are monotonic acknowledgements, not event counters. A client configuration reset clears both acknowledgements, and delayed presentations may never move either value backwards. Bootstrap and recovery policy compares them with the required server ownership epoch before changing transport ownership. The same feedback includes video decode-queue drops plus the current audio playback state (`disabled`, `buffering`, `playing`, or `starved`), the current startup-hold age, and a timeout counter. These fields let the server distinguish a bounded A/V startup wait from a decode or presentation failure.


## Handshake and auxiliary-channel invariants

The control connection begins with exactly one `WD_MSG_CLIENT_HELLO`. The frame may be split across any number of TCP reads; implementations therefore use the incremental reader and enforce both an idle timeout and an absolute frame-lifetime timeout.

Input, selection, video, and audio sockets are bound to the control session by their channel-hello payload. A channel is accepted only when its session ID and connection token match the active control connection, the channel has not already been bound, and any negotiated codec/transport fields match. Established traffic remains channel-specific; for example, primary-selection messages are never valid on the control socket.


## Protocol descriptor completeness

The dispatch descriptor table is the canonical route and size policy for every message type. Tests enumerate the complete contiguous protocol-zero message range, verify fixed/empty/opaque/repeated size boundaries, prove that each descriptor has at least one legal channel/phase/direction route, and confirm that per-channel payload caps cover every legal message. Coverage-guided fuzz inputs are isolated: mutable reassembly state is recreated for each input so failures reproduce independently of corpus execution order.


Client video packet validation is shared between the production receive loop and unit tests. Non-control frames must match the active session identity and configured visible geometry, and coded dimensions cannot be smaller than visible dimensions. Resize/end-of-stream controls may announce transition geometry with an empty payload; ordinary empty frames are invalid.
