# AR-AI-Glasses

面向可穿戴 AR 设备的多模态实时交互原型。摄像头与麦克风同时采集,本地完成语音转写,交给云端视觉模型推理,再通过 DRM/KMS 把回答直接绘制到屏幕上——全程不依赖任何窗口系统。

```
                  ┌─────────────────────┐
   按下 s   ──────┤  并行采集 (10 秒)   │
                  │  ├ V4L2  → JPEG     │──┐
                  │  └ ALSA  → whisper  │──┤
                  └─────────────────────┘  │
                                           ▼
                             ┌──────────────────────────┐
                             │ 打包 OpenAI 多模态 JSON  │
                             │ base64 图序列 + 转写文本 │
                             └──────────────────────────┘
                                           │  HTTPS POST
                                           ▼
                             ┌──────────────────────────┐
                             │  云端视觉语言模型推理    │
                             └──────────────────────────┘
                                           │  回答文本
                                           ▼
                             ┌──────────────────────────┐
                             │ DRM/KMS + GBM + EGL      │
                             │ FreeType 直接画到扫描缓冲│
                             └──────────────────────────┘
```

---

## 目录

- [特性](#特性)
- [硬件与系统要求](#硬件与系统要求)
- [依赖安装](#依赖安装)
- [编译](#编译)
- [配置](#配置)
- [运行](#运行)
- [项目结构](#项目结构)
- [设计说明](#设计说明)
- [故障排查](#故障排查)
- [已知限制](#已知限制)
- [开发约定](#开发约定)
- [许可](#许可)

---

## 特性

| | 说明 |
|---|---|
| **零窗口系统依赖** | 直接操作 DRM/KMS,不需要 X11 或 Wayland,适合资源紧张的嵌入式设备 |
| **视觉与语音并行采集** | 两个独立硬件同时工作,10 秒完成而非串行的 20 秒 |
| **语音本地转写** | whisper.cpp 在设备侧完成 STT,语音音频不出设备 |
| **自动发现显示通路** | 运行时枚举 DRM 资源找 connector/CRTC,换屏幕不用改代码 |
| **中英混排自动折行** | 按 CJK/拉丁两套规则断行,并避免标点顶行首 |
| **配置全外置** | 设备节点、模型路径、密钥、显示参数均可由环境变量或配置文件指定 |

---

## 硬件与系统要求

| 项目 | 要求 |
|---|---|
| 操作系统 | Linux(需要 V4L2、ALSA、DRM/KMS 三套内核接口) |
| 摄像头 | 支持 V4L2 且能输出 **YUYV** 格式(绝大多数 USB 摄像头都可以) |
| 麦克风 | 任意 ALSA 可见的录音设备 |
| 显示 | 一个真实的 KMS 输出。**无法在纯 SSH 会话或无头环境下出图** |
| 编译器 | 支持 C++17(GCC 8+ / Clang 7+) |
| CMake | 3.16 及以上 |

> **关于权限**:程序需要读写 `/dev/video*`、`/dev/dri/*` 和录音设备。推荐把用户加进相应用户组而不是直接 `sudo`:
> ```bash
> sudo usermod -aG video,audio,render $USER
> # 重新登录后生效
> ```

---

## 依赖安装

### Debian / Ubuntu / Raspberry Pi OS

```bash
sudo apt update
sudo apt install -y \
    build-essential cmake pkg-config \
    libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev \
    libasound2-dev libcurl4-openssl-dev libjpeg-dev libfreetype6-dev \
    nlohmann-json3-dev fonts-noto-cjk \
    v4l-utils alsa-utils
```

`fonts-noto-cjk` 是显示中文的必需项——只装拉丁字体的话中文会全部渲染成空白。
`v4l-utils` 和 `alsa-utils` 只在排查设备问题时用到(`v4l2-ctl`、`arecord`),但强烈建议装上。

### Arch Linux

```bash
sudo pacman -S --needed \
    base-devel cmake pkgconf \
    libdrm mesa alsa-lib curl libjpeg-turbo freetype2 \
    nlohmann-json noto-fonts-cjk v4l-utils alsa-utils
```

### whisper.cpp

本项目链接 whisper.cpp 的共享库,需要先单独编译:

```bash
git clone https://github.com/ggerganov/whisper.cpp
cd whisper.cpp
cmake -B build -DBUILD_SHARED_LIBS=ON
cmake --build build -j$(nproc)

# 下载模型。base 约 142MB,在树莓派这类设备上是速度和准确率的合理折中;
# 追求准确率可换 small(~466MB),追求速度可换 tiny(~75MB)。
bash models/download-ggml-model.sh base

# 记下这两个路径,后面要用
pwd                                  # -> WHISPER_ROOT
ls models/ggml-base.bin              # -> whisper.model_path
```

---

## 编译

```bash
git clone https://github.com/Heathcliff3302/AR-AI-Glasses
cd AR-AI-Glasses

cmake -B build -DWHISPER_ROOT=/绝对路径/whisper.cpp
cmake --build build -j$(nproc)
```

产物是 `build/ar-ai-glasses`。

`WHISPER_ROOT` 也可以走环境变量,或者在 whisper 已经装进系统路径时完全省略:

```bash
export WHISPER_ROOT=/绝对路径/whisper.cpp
cmake -B build
```

调试构建:

```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DWHISPER_ROOT=/路径/whisper.cpp
cmake --build build-debug -j
```

---

## 配置

所有参数的读取优先级是:

```
环境变量  >  config.ini  >  程序内置默认值
```

环境变量名的规则是「`AR_` + 键名大写 + 点改下划线」,例如 `api.token` → `AR_API_TOKEN`。

### 最小启动配置

只有两项是**必填**的,其余都有可用默认值:

| 配置项 | 环境变量 | 说明 |
|---|---|---|
| `api.token` | `AR_API_TOKEN` | 云端模型的 API 密钥 |
| `whisper.model_path` | `AR_WHISPER_MODEL_PATH` | ggml 模型文件的路径 |

所以最快的跑法是不建配置文件,直接:

```bash
export AR_API_TOKEN='sk-你的密钥'
export AR_WHISPER_MODEL_PATH=/路径/whisper.cpp/models/ggml-base.bin
./build/ar-ai-glasses
```

### 使用配置文件

```bash
cp config.example.ini config.ini
$EDITOR config.ini
```

`config.example.ini` 里逐项写了注释和取值建议。`config.ini` 已列入 `.gitignore`。

配置文件也可以放在别处:

```bash
AR_CONFIG_FILE=/etc/ar-glasses/prod.ini ./build/ar-ai-glasses
```

### ⚠️ 关于 API 密钥

**密钥请优先用环境变量,不要写进任何会被提交的文件。**

如果密钥曾经出现在某次 git 提交里,那么删掉文件、重写当前版本都是**无效**的——它依然存在于历史对象中,任何人 clone 下来都能用 `git log -S` 翻出来。唯一正确的处置是**去服务商控制台吊销该密钥并重新签发**。

---

## 运行

```bash
./build/ar-ai-glasses
```

启动后会先打印检测到的显示通路、字体和模型信息,然后进入交互循环:

```
[配置] 已加载 config.ini
[AI] 模型=Qwen/Qwen3-VL-32B-Thinking token=sk-a...z9f1
[显示] connector=208 crtc=71 模式=1920x1080@60Hz
[显示] 使用字体 /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc

就绪。按 s + 回车 开始一次采集,按 q + 回车 退出。
```

| 按键 | 动作 |
|---|---|
| `s` + 回车 | 开始一次完整交互(采集 → 推理 → 显示) |
| `q` + 回车 | 退出 |
| `Ctrl+C` | 同样走正常退出路径,会还原屏幕状态 |

一次交互的典型输出:

```
[音频] 录音 10 秒 (采样率 16000 Hz)...
[采集] 图像 20 帧,识别文本: "这个零件应该装在哪里"
[AI] 请求体 1043 KB,图片 20 张
[AI] 回答: 画面中的金属支架应安装在...
[显示] 已上屏
```

单次交互失败不会退出程序:错误会打到终端、同时显示在屏幕上,等待下一次按键。只有初始化阶段的失败才会以非零码退出。

---

## 项目结构

```
AR-AI-Glasses/
├── CMakeLists.txt          构建脚本
├── config.example.ini      配置模板(逐项带注释)
├── include/
│   ├── config.h            配置读取:环境变量 + ini 文件
│   ├── base64.h            base64 编码
│   ├── camera.h            V4L2 采集 → JPEG → base64
│   ├── audio.h             ALSA 录音 + whisper 转写
│   ├── ai_client.h         HTTP 客户端:请求组装、重试、响应解析
│   └── display.h           DRM/KMS + GBM + EGL + FreeType 文字渲染
├── src/
│   ├── main.cpp            交互循环与线程编排
│   ├── config.cpp
│   ├── base64.cpp
│   ├── camera.cpp
│   ├── audio.cpp
│   ├── ai_client.cpp
│   └── display.cpp
├── LICENSE
└── README.md
```

每个模块是一个独立的 RAII 类,资源在构造时获取、析构时释放,构造失败不留残留状态。模块之间只通过 `Config` 和 `std::string` / `std::vector` 交互,没有全局可变状态。

---

## 设计说明

这一节记录几个不那么显然的技术选择,方便后续修改时不踩回同一个坑。

### 为什么直接向 ALSA 请求 16 kHz

whisper 要求 16 kHz 单声道输入。一个容易犯的错误是按设备默认的 48 kHz 采集,然后「每 3 个样本取 1 个」降到 16 kHz——这是**没有抗混叠滤波的抽取**,源信号中 8 kHz 以上的成分会折返到可听频段,叠加成类似金属噪声的干扰,直接拉低识别准确率。

正确做法有两种,本项目两种都用:

1. **首选**:用 `snd_pcm_hw_params_set_rate_near` 直接让硬件输出 16 kHz,根本不需要重采样。
2. **兜底**:硬件确实给不了 16 kHz 时,先用窗函数法设计的 101 阶 FIR 低通(截止 7.2 kHz、Hamming 窗)滤掉高频,**再**重采样。

代码里只在需要输出的位置上做卷积,省掉被丢弃样本的滤波开销。

### 为什么用 drmModeSetCrtc 而不是 drmModeSetPlane

`drmModeSetPlane` 需要事先知道一个可用的 overlay plane ID。这个 ID 随硬件和内核版本变化,没法写死,而正确枚举 plane 又要处理 `possible_crtcs` 掩码和格式能力。`drmModeSetCrtc` 只需要 connector 和 CRTC,这两者可以通过标准的资源枚举流程可靠地发现,可移植性好得多。

代价是占用整个 CRTC 而非叠加在某一层上。真正要做半透明 AR 叠加层时,需要换成 atomic modeset 配合正确选出的 overlay plane。

### 字形缓存与纹理格式

字形位图上传为单通道 `GL_ALPHA` 纹理,颜色由 uniform 提供,而不是把灰度值复制成 RGBA 四个通道——后者的显存占用和上传带宽都是 4 倍。

字形按码点缓存。中文一屏可能有两三百字,不缓存的话每帧要做几百次 `glGenTextures`/`glTexImage2D`/`glDeleteTextures`,而汉字复用率其实很高。

纹理必须设 `GL_CLAMP_TO_EDGE`:否则线性采样会在字形边缘取到对侧像素,表现为每个字四周有一圈淡淡的重影。

### 图像帧数与 token 消耗

请求体大小和 token 消耗随帧数线性增长。默认 10 秒 / 500 ms = 约 20 帧,`image_detail` 默认取 `low`。

如果调成 `high`,20 张图很容易撞上单次请求的 token 上限,而服务端返回的往往只是一个语焉不详的 400。想提高单帧清晰度时,更稳的做法是同时减少帧数。

### 并发结构

图像和音频的采集是两个独立硬件,串行要 20 秒、并行只要 10 秒,所以各起一个线程。

同步用「互斥量 + 条件变量 + 谓词」的标准组合:通知在持锁期间发出,`cv.wait` 带谓词以防虚假唤醒,`join` 之后主线程才读共享数据。线程内抛出的异常存进 `std::exception_ptr`,回到主线程再 rethrow——异常无法跨线程边界自动传播,不接住就是 `std::terminate`。

### 错误处理的分层

- **初始化失败**(打不开设备、找不到模型、密钥缺失)→ 抛异常,`main` 捕获后打印带排查提示的信息并返回 1。
- **单次交互失败**(网络抖动、限流、摄像头被占用)→ 抛异常,交互循环捕获后同时打到终端和屏幕,程序继续等下一次按键。
- **单帧失败**(某一帧 JPEG 压缩出错)→ 就地跳过,不影响整次采集。

库层面有两处需要特别拦截,否则它们会直接杀掉进程:libjpeg 的 `error_exit` 默认调用 `exit(1)`,通过接管回调 + `longjmp` 转成可捕获的异常;curl 的超时在多线程下需要 `CURLOPT_NOSIGNAL`,否则它用信号实现超时会干扰其他线程。

---

## 故障排查

### 编译期

| 报错 | 原因与处置 |
|---|---|
| `找不到 whisper.cpp` | 没传 `-DWHISPER_ROOT=`,或该目录下没有编译好的 `libwhisper.so`。确认 whisper.cpp 是用 `-DBUILD_SHARED_LIBS=ON` 编的 |
| `找不到 nlohmann/json.hpp` | `apt install nlohmann-json3-dev` |
| `None of the required 'libdrm' found` | 缺 `libdrm-dev`,参照[依赖安装](#依赖安装)补齐 |

### 运行期

| 现象 | 排查方向 |
|---|---|
| `缺少必填配置项 "api.token"` | `export AR_API_TOKEN=...`,或填进 `config.ini` |
| `打开摄像头 /dev/video0 失败: Permission denied` | 用户不在 `video` 组,或设备节点不对。`v4l2-ctl --list-devices` 确认 |
| `摄像头不支持 YUYV` | 设备只出 MJPEG。本程序目前不解码 MJPEG,需要自行添加解码分支 |
| `打开录音设备 default 失败` | `arecord -l` 列出设备,把 `audio.device` 改成 `hw:<卡号>,<设备号>` |
| `没找到已连接的显示器` | 屏幕没插好,或当前在 SSH/无头环境——本模块需要真实 KMS 输出 |
| `找不到可用字体` | `apt install fonts-noto-cjk`,或用 `display.font_path` 指定 |
| 中文显示成空白方框 | 字体没有中文字形(比如落到了 DejaVu 兜底项)。装中文字体并显式指定路径 |
| `认证失败(HTTP 401)` | 密钥错误、过期或已被吊销 |
| 语音识别准确率很低 | 确认日志里打印的采样率是 16000;麦克风离得太远或环境噪声大也会显著影响 |
| 退出后屏幕一直黑 | 正常退出会自动还原 CRTC。若是被 `kill -9` 强杀则跳过了析构,`sudo chvt 1; sudo chvt 7` 可恢复 |

---

## 已知限制

坦白列出来,避免误解项目的成熟度:

- **只支持 YUYV 摄像头**。只输出 MJPEG 或 H.264 的设备需要自行加解码。
- **占用整个 CRTC**,不是真正的半透明叠加层。做真 AR 叠加需要改用 atomic modeset + overlay plane。
- **显示是静态单帧**,没有页翻转循环,不适合动画或视频叠加。
- **一屏放不下的回答会被截断**并显示提示,没有翻页。
- **没有自动化测试**。纯算法部分(base64、UTF-8 解码、折行、重采样)是可测的,但目前还没有测试用例。
- **交互靠终端按键**,不是可穿戴设备该有的形态(按钮、手势、语音唤醒)。
- **整段流程是一次性请求**,不是流式的持续感知。真正的实时交互需要改成滑动窗口 + 增量推理。

---

## 开发约定

- **提交前必须编译通过。** 这是最低要求。
  ```bash
  cmake --build build -j && echo "OK"
  ```
- **不提交密钥。** 任何形如 `sk-` 的字符串都不该进版本库。可以装个 pre-commit 钩子兜底:
  ```bash
  cat > .git/hooks/pre-commit <<'HOOK'
  #!/bin/sh
  if git diff --cached | grep -qE 'sk-[A-Za-z0-9]{20,}'; then
      echo "拒绝提交: 检测到疑似 API 密钥" >&2
      exit 1
  fi
  HOOK
  chmod +x .git/hooks/pre-commit
  ```
- **不引入新的硬编码路径或设备 ID。** 一切环境相关的值走 `Config`。
- **编译警告视为错误。** 构建已开 `-Wall -Wextra -Wpedantic`,新增代码不应引入警告。
- **注释解释「为什么」,不复述「做了什么」。** `// 初始化 curl 对象` 这种注释没有价值;说明为什么必须设 `CURLOPT_NOSIGNAL` 才有。

---

## 许可

MIT,详见 [LICENSE](LICENSE)。

### 第三方组件

| 组件 | 用途 | 许可 |
|---|---|---|
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | 本地语音转写 | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON 序列化 | MIT |
| libcurl / libjpeg-turbo / FreeType | HTTP、JPEG、字体渲染 | 各自的宽松许可 |
| libdrm / Mesa (GBM, EGL, GLES2) | 显示输出 | MIT |
| ALSA | 音频采集 | LGPL-2.1 |
