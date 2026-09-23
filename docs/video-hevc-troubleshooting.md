# HEVC trace checklist

When HEVC stalls but H.264 runs, collect simultaneous client/server INFO logs
with the same session. `video trace` samples frames 1–8 and then every 128th
frame **per encoder frame-ID sequence** (and only on the current client video
TCP channel). It is on by default for these sparse samples; no payload bytes
are dumped. Packet hashes are diagnostic only, not authentication.

Compare `epoch`, `frame`, `bytes`, `prefix`, and `hash` in `server-send` and
`client-recv`. An identical fingerprint does not prove all bytes match
cryptographically. A difference proves the copies are different, except in
case of a hash collision. `server-drop` means the frame was encoded but not
transmitted; the encoder is instructed to emit a keyframe next.

Then compare `client-dequeue` queue wait, `client-decode` duration and output,
`client-publish` queue depth, and `client-upload`/`client-present`. An absent
stage localizes the first transition that stopped. Frame IDs attached to
`client-decode`'s output may lag the input frame ID due to codec buffering.
`encode_ms` includes codec reconfiguration, and `queue_ms` is elapsed time
since the server worker job was published (not TCP one-way network latency).

The log is intentionally sampled and is **not** a complete per-frame transport
trace. Use `--video-codec h264` as the temporary workaround if HEVC remains
unstable. Do not increase the decode queue simply to suppress overflow; that
can increase latency instead of recovering correct reference frames.

## Distinguish discard from slow decoder initialization

`client-dequeue` occurs **before** content-epoch / transition checks and decoder
configuration, so it does not prove a packet reached FFmpeg. Check for the
following sampled stages for the *same* `epoch` and `frame`:

- `client-handle` then `client-drop`: the message was rejected before decoding;
  the `reason` identifies the guard. No payload or connection token is logged.
- `client-epoch`: time spent accepting the content-epoch/ownership transition;
  a missing line after `client-handle` indicates this stage is blocked.
- `client-configure-start` / `client-configure-end`: decoder setup time and
  success. A long interval here can fill the four-packet input queue even if
  subsequent `client-decode` calls take only milliseconds.
- `client-decode`, `client-publish`, `client-upload`, `client-present`: the
  original stages remain unchanged. Compare the server's matching
  `server-send` lines to avoid confusing an intentional repeat keyframe with
  a lost frame.

On overflow, `oldest` and `oldest_wait_ms` are captured *before* the input
queue is flushed. These identify the oldest **queued** packet, not the packet
already being processed by the decode worker. To distinguish those, pair the
overflow with the most recent `client-dequeue`/`client-configure-start` line.

For deeper diagnostics without rebuilding with CMake manually, run
`WAYDISPLAY_PACKAGE_LOG_LEVEL=DEBUG makepkg -sif` from the checkout root.
This compiles DEBUG logging into the **Release binaries** while retaining
Release optimization and Arch hardening flags. The separate Debug test tree
always uses DEBUG. A normal `makepkg -sif` reconfigures Release back to INFO.
The package release number is unchanged by this optional diagnostic switch;
use it only for a local package, not a distributed reproducible build.
