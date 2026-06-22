# Video decoder fixtures

The `64x48` fixtures exercise software decode and visible-frame cropping. The
`128x128` fixtures exercise VAAPI decode using dimensions accepted by hardware
that rejects sub-64-pixel coded heights.

The fixtures are one-frame Annex B elementary streams generated from FFmpeg's
`testsrc2` source:

```sh
ffmpeg -f lavfi -i 'testsrc2=size=128x128:rate=1' -frames:v 1 \
  -vf format=yuv420p -c:v libx264 -profile:v constrained_baseline \
  -preset ultrafast -tune zerolatency -f h264 video_keyframe_128x128.h264

ffmpeg -f lavfi -i 'testsrc2=size=128x128:rate=30' -frames:v 1 \
  -vf format=yuv420p -c:v libx265 -profile:v main -preset ultrafast \
  -tune zerolatency \
  -x265-params 'repeat-headers=1:bframes=0:pools=none:frame-threads=1:log-level=error' \
  -f hevc video_keyframe_128x128.h265
```

Keep the VAAPI fixtures in broadly supported 8-bit 4:2:0 profiles, at least
`128x128`, and without B-frames. Hardware decoders are allowed to pipeline the
first access unit, but the fixture itself should not add codec reordering delay.
These files test the hardware path rather than minimum coded-size limits.
