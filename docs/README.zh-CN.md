# Floral 硬件编解码器

`hardware_floral_codec` 提供由 FFmpeg 和 VA-API 驱动的非安全 Android Codec2
组件。vendor component store 会在服务启动时探测当前 render node，仅发布 FFmpeg
后端和 VA-API 驱动同时支持的编解码方向。

首批格式为 AVC、HEVC、VP8、VP9、AV1 和 MPEG-2。编码与解码分别探测，只暴露
8-bit profile；Android 原有的软件编解码器会继续保留作为回退。

## 运行行为

默认 render node 是 `/dev/dri/renderD128`，可通过以下参数修改：

```text
androidboot.floral_vaapi_device=/dev/dri/renderD129
```

无需额外的编解码器启用参数。容器需要获得 render node，常见配置为
`--device /dev/dri:/dev/dri` 和 `androidboot.floral_gpu_mode=host`。

`androidboot.floral_video_encoder` 仍然只控制 Floral socket 串流的专用编码器，
不会启用或关闭 Android MediaCodec 组件。

可使用以下命令列出已经注册的组件：

```bash
adb shell dumpsys media.player | grep -F c2.floral
```

应用也可以明确指定组件。例如新版 scrcpy 可使用
`--video-encoder=c2.floral.avc.encoder`。

## 限制

当前不实现受 DRM 保护的输入和 `video/*.secure` 组件。在 P010 等 Android 10-bit
GraphicBuffer 链路完整实现前，也不会发布 10-bit 能力。
