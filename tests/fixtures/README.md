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

The `video_delayed_64x48` fixtures contain twelve Annex B access units with
B-frame reordering.  Their adjacent `.manifest` files record each access-unit
size and its encoder presentation timestamp in microseconds.  Decoder tests
submit the packets in decode order and require returned metadata to follow
presentation order, which catches associating a delayed frame with the newest
packet instead of the packet that produced it.

They were generated through an MP4 intermediate so packet presentation times
could be recorded before converting the packets to Annex B:

```sh
ffmpeg -f lavfi -i 'testsrc2=size=64x48:rate=4' -frames:v 12 \
  -vf format=yuv420p -c:v libx264 -preset medium -g 12 -bf 2 \
  -x264-params 'aud=1:keyint=12:min-keyint=12:scenecut=0' delayed-h264.mp4
ffmpeg -i delayed-h264.mp4 -map 0:v:0 -c copy \
  -bsf:v h264_mp4toannexb -f h264 video_delayed_64x48.h264

ffmpeg -f lavfi -i 'testsrc2=size=64x48:rate=4' -frames:v 12 \
  -vf format=yuv420p -c:v libx265 -preset medium -g 12 -bf 2 \
  -x265-params 'aud=1:keyint=12:min-keyint=12:scenecut=0:pools=none:frame-threads=1:log-level=error' \
  -tag:v hvc1 delayed-h265.mp4
ffmpeg -i delayed-h265.mp4 -map 0:v:0 -c copy \
  -bsf:v hevc_mp4toannexb -f hevc video_delayed_64x48.h265
```
