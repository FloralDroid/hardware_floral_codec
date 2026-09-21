# Floral hardware codecs

[简体中文](README.zh-CN.md)

`hardware_floral_codec` provides non-secure Android Codec2 components. x86_64
uses FFmpeg with VA-API; ARM64 uses the kernel V4L2 M2M interface exposed by
Qualcomm Venus. The vendor component store probes devices at service startup
and publishes only codec directions supported by the selected driver.

x86_64 probes AVC, HEVC, VP8, VP9, AV1, and MPEG-2 encoders and decoders.
ARM64 probes AVC, HEVC, and VP8 in both directions, plus VP9 and MPEG-2
decoders. Each direction is registered independently. Only 8-bit profiles are
exposed; the software Android codecs remain available.

On ARM64, encoder Surface input is imported as a DRM DMA-BUF and converted by
Vulkan compute directly into the linear NV12 DMA-BUF queued to Venus. Decoder
capture buffers are Codec2 GraphicBlocks queued directly to Venus. Video frame
pixels are not mapped or copied by the CPU on these paths.

## Runtime behavior

x86_64 scans available `/dev/dri/renderD*` nodes. A deployment can restrict
probing to one node with:

```text
androidboot.floral_vaapi_device=/dev/dri/renderD129
```

ARM64 scans `/dev/video*`. Probing can be restricted to one node with
`androidboot.floral_v4l2_device=/dev/video12`; components for unsupported
directions on that node are not registered.

No codec enable parameter is required. The container must receive the relevant
render or video nodes. x86_64 normally uses `--device /dev/dri:/dev/dri` and
`androidboot.floral_gpu_mode=host`; ARM64 needs the Venus `/dev/video*` nodes.

`androidboot.floral_video_encoder` continues to select only the private Floral
socket-stream encoder. It does not enable or disable Android MediaCodec
components.

List registered codecs with:

```bash
adb shell dumpsys media.player | grep -F c2.floral
```

Applications may request a component explicitly. For example, recent scrcpy
versions can use `--video-encoder=c2.floral.avc.encoder`.

## Limits

Protected DRM input and `video/*.secure` components are intentionally not
implemented. P010 and other 10-bit Android GraphicBuffer paths are also not
advertised until their buffer handling is complete. The ARM64 zero-copy path
currently requires minigbm buffers, Vulkan DMA-BUF and DRM format modifier
support, and linear NV12 support from Venus.
