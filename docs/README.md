# Floral hardware codecs

`hardware_floral_codec` provides non-secure Android Codec2 components backed by
FFmpeg and VA-API. The vendor component store probes the active render node at
service startup and publishes only codec directions supported by both FFmpeg
and the VA-API driver.

The initial format set is AVC, HEVC, VP8, VP9, AV1, and MPEG-2. Encode and
decode support are probed independently. Only 8-bit profiles are exposed; the
software Android codecs remain installed as a fallback.

## Runtime behavior

The default render node is `/dev/dri/renderD128`. It can be changed with:

```text
androidboot.floral_vaapi_device=/dev/dri/renderD129
```

No codec enable parameter is required. The container must receive its render
node, normally with `--device /dev/dri:/dev/dri` and
`androidboot.floral_gpu_mode=host`.

`androidboot.floral_video_encoder` continues to select only the private Floral
socket-stream encoder. It does not enable or disable Android MediaCodec
components.

List registered codecs with:

```bash
adb shell dumpsys media.codec | grep -F c2.floral
```

Applications may request a component explicitly. For example, recent scrcpy
versions can use `--video-encoder=c2.floral.avc.encoder`.

## Limits

Protected DRM input and `video/*.secure` components are intentionally not
implemented. P010 and other 10-bit Android GraphicBuffer paths are also not
advertised until their buffer handling is complete.
