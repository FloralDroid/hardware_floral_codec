# Floral 硬件编解码器

[English](README.md)

`hardware_floral_codec` 提供非安全 Android Codec2 组件。x86_64 使用 FFmpeg 和
VA-API，ARM64 使用 Qualcomm Venus 暴露的内核 V4L2 M2M 接口。vendor component
store 会在服务启动时探测设备，只发布所选驱动实际支持的编解码方向。

x86_64 会探测 AVC、HEVC、VP8、VP9、AV1 和 MPEG-2 编解码器。ARM64 会探测
AVC、HEVC、VP8 编码和解码，以及 VP9、MPEG-2 解码。每个方向独立注册，只暴露
8-bit profile；Android 原有的软件编解码器仍然可用。

ARM64 编码时通过 DRM DMA-BUF 导入 Surface 输入，由 Vulkan compute 将 RGB
直接写入排入 Venus 的线性 NV12 DMA-BUF。解码时把 Codec2 GraphicBlock 直接排入
Venus capture 队列。这两条视频帧路径都不在 CPU 上映射或复制像素。

## 运行行为

x86_64 会探测可用的 `/dev/dri/renderD*` 节点，也可以通过以下参数限制为单个节点：

```text
androidboot.floral_vaapi_device=/dev/dri/renderD129
```

ARM64 会探测 `/dev/video*`。可使用
`androidboot.floral_v4l2_device=/dev/video12` 限制为单个节点；该节点不支持的方向
不会注册对应组件。

无需额外的编解码器启用参数。容器必须获得相应的 render 或 video 节点。x86_64
常见配置为 `--device /dev/dri:/dev/dri` 和
`androidboot.floral_gpu_mode=host`；ARM64 需要传入 Venus 的 `/dev/video*` 节点。

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
GraphicBuffer 链路完整实现前，也不会发布 10-bit 能力。ARM64 零拷贝路径目前要求
使用 minigbm 缓冲区，Vulkan 支持 DMA-BUF 和 DRM format modifier，并且 Venus
支持线性 NV12。
