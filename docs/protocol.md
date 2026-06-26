# WayDisplay protocol

WayDisplay currently uses protocol version 0. The protocol may change without backward compatibility while the project remains undeployed.

## Platform contract

Both peers must run on little-endian Linux hosts. Big-endian and non-Linux systems are intentionally unsupported; configuration fails on such targets. The TCP wire layout is nevertheless encoded and decoded explicitly. Compiler structure padding, alignment, and native ABI layout are not transmitted.

The expected deployment is localhost, a trusted network, or a VPN. Protocol version 0 does not provide authentication or encryption.
