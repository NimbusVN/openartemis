# 介绍

openartemis的核心源码仓库，基于 SDL3 实现的 Artemis 系视觉小说引擎，直接运行 PFS 资源包（`root.pfs`）。源码构建在 [NimbusVN](https://github.com/NimbusVN/NimbusVN) 完成。

# 目录结构说明

```
📁 core/        # 引擎内核
├── 📁 runtime/ # 运行时：App、场景树、脚本(Lua)、存档、IET/UI 事件
├── 📁 render/  # 渲染：图层/字体/文本、SDL 与 GLES 后端
├── 📁 media/   # 媒体：图片/音频/视频解码与解码池
├── 📁 emote/   # E-mote 动态立绘（PSB 读取、骨骼、物理）
├── 📁 fs/      # 文件系统：PFS 包与 PhysFS 抽象
└── 📁 util/    # 工具包（sha1/charset/binary_stream）
📁 environ/     # 宿主层：main.cpp + platform/（desktop / android / wasm）
📁 third_party/ # vendored Lua 5.1.5
```

# 依赖库说明

- sdl3:跨平台核心（窗口/输入/音频/渲染）
- lua 5.1.5:脚本语言（vendored 源码）
- physfs:资源包文件系统
- zlib:基础压缩算法
- ffmpeg:视频解码（可选，未找到时自动关闭）
- libvorbis/libtheora:音频与视频解码
- libpng/libjpeg-turbo:图片解码
- freetype:字体渲染

# 宏定义说明

- OA_HAVE_FFMPEG:启用 ffmpeg 视频后端。
- __ANDROID__/__EMSCRIPTEN__/__IPHONEOS__/桌面:平台差异由编译器自带宏区分。

# 许可

**MIT 或 GPL-3.0-or-later 二选一**（`SPDX-License-Identifier: MIT OR GPL-3.0-or-later`），
Copyright (c) 2026 deepseek，详见 `LICENSE`。代码不含任何游戏资产。
