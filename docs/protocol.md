# WayDisplay protocol

WayDisplay currently uses protocol version 0. The protocol may change without backward compatibility while the project remains undeployed.

## Platform contract

Both peers must run on little-endian Linux hosts. Big-endian and non-Linux systems are intentionally unsupported; configuration fails on such targets. Protocol zero sends packed, fixed-width C structures directly. GCC-compatible one-byte packing, little-endian field order, and the compile-time asserted structure sizes are the current wire ABI. Compatibility with other compilers, architectures, or future protocol revisions is not promised. Variable media and selection tails remain opaque byte sequences.

The expected deployment is localhost, a trusted network, or a VPN. Protocol version 0 does not provide authentication or encryption.
