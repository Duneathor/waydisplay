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
