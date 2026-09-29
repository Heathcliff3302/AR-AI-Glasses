#include "camera.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include <jpeglib.h>

#include "base64.h"

namespace ar {
namespace {

std::string errnoText(const char* what) {
  return std::string(what) + " 失败: " + std::strerror(errno);
}

// ioctl 被信号打断是常态(程序里还有别的线程在跑)，这里统一重试。
// 不处理 EINTR 的话，偶发的中断会被当成真错误，表现出来就是
// 「偶尔采集失败」这种极难复现的 bug。
int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do {
    r = ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint8_t clampByte(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// YUYV(YUV 4:2:2 打包) 转 RGB24。
// 一个 YUYV 宏块是 4 字节 Y0/U/Y1/V，描述横向相邻的两个像素：
// 两个像素各有自己的亮度 Y，但共用一组色度 U/V。
// 系数是 BT.601 的定点化版本(乘 256 后取整)，用整数移位代替浮点，
// 在没有 FPU 优势的嵌入式 CPU 上更快。
void yuyvToRgb24(const uint8_t* src, size_t src_stride, uint8_t* dst, int width, int height) {
  const size_t dst_stride = static_cast<size_t>(width) * 3;
  for (int y = 0; y < height; ++y) {
    const uint8_t* s = src + static_cast<size_t>(y) * src_stride;
    uint8_t* d = dst + static_cast<size_t>(y) * dst_stride;
    for (int x = 0; x + 1 < width; x += 2) {
      const int y0 = s[0], u = s[1], y1 = s[2], v = s[3];
      const int du = u - 128;
      const int dv = v - 128;

      int c = y0 - 16;
      d[0] = clampByte((298 * c + 409 * dv + 128) >> 8);
      d[1] = clampByte((298 * c - 100 * du - 208 * dv + 128) >> 8);
      d[2] = clampByte((298 * c + 516 * du + 128) >> 8);

      c = y1 - 16;
      d[3] = clampByte((298 * c + 409 * dv + 128) >> 8);
      d[4] = clampByte((298 * c - 100 * du - 208 * dv + 128) >> 8);
      d[5] = clampByte((298 * c + 516 * du + 128) >> 8);

      s += 4;
      d += 6;
    }
  }
}

// libjpeg 出错时的默认行为是打印一行然后 exit(1)，会把整个程序带走。
// 标准解法是接管 error_exit，用 longjmp 跳回调用点，
// 把一个「进程级死亡」变成一个能 catch 的普通错误。
//
// 这个结构体连错误信息和输出缓冲一起放，并且整体分配在堆上。
// 原因是 setjmp/longjmp 有一条容易忽略的规则：longjmp 返回之后，
// 调用 setjmp 那个函数里「非 volatile 且在 setjmp 之后被改过」的局部变量，
// 取值是不确定的。如果把 libjpeg 分配的输出指针放在局部变量里，
// 错误路径上 free 它就是在 free 一个不确定的值。
// 而只要指向本结构体的指针在 setjmp 之前就确定且之后不再变，
// 堆上的内容就不受这条规则约束，读写都是良定义的。
struct JpegSession {
  struct jpeg_error_mgr pub;  // 必须是第一个成员：cinfo.err 会指向这里
  std::jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
  unsigned char* data = nullptr;  // libjpeg 用 malloc 分配，要用 free 还
  unsigned long size = 0;

  JpegSession() { message[0] = '\0'; }
  ~JpegSession() {
    if (data) free(data);
  }
};

void jpegErrorExit(j_common_ptr cinfo) {
  auto* s = reinterpret_cast<JpegSession*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, s->message);
  std::longjmp(s->jump, 1);
}

}  // namespace

Camera::Camera(const Config& cfg) {
  device_ = cfg.getString("camera.device", "/dev/video0");
  const int want_w = cfg.getInt("camera.width", 640);
  const int want_h = cfg.getInt("camera.height", 480);
  interval_ms_ = cfg.getInt("camera.interval_ms", 500);
  duration_ms_ = cfg.getInt("camera.duration_ms", 10000);
  jpeg_quality_ = cfg.getInt("camera.jpeg_quality", 85);
  const int want_buffers = cfg.getInt("camera.buffer_count", 4);

  if (interval_ms_ <= 0) throw std::runtime_error("camera.interval_ms 必须大于 0");
  if (duration_ms_ <= 0) throw std::runtime_error("camera.duration_ms 必须大于 0");
  if (jpeg_quality_ < 1 || jpeg_quality_ > 100) {
    throw std::runtime_error("camera.jpeg_quality 必须在 1..100 之间");
  }

  // O_NONBLOCK 配合 select 用：阻塞式 DQBUF 在摄像头不出帧时会永久卡住，
  // 旧版就是这样把采集线程挂死，导致主线程的条件变量永远等不到通知。
  fd_ = open(device_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    throw std::runtime_error("打开摄像头 " + device_ + " 失败: " + std::strerror(errno) +
                             "\n  提示: 用 v4l2-ctl --list-devices 确认节点，"
                             "并确保当前用户在 video 组内");
  }

  // 下面任何一步失败都必须回收 fd 和已经映射好的 buffer。
  // 用 try/catch 统一收口，比在每个失败分支前手写一遍清理更不容易漏。
  try {
    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
      throw std::runtime_error(errnoText("VIDIOC_QUERYCAP"));
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
      throw std::runtime_error(device_ + " 不是视频采集设备");
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
      throw std::runtime_error(device_ + " 不支持 mmap 流式采集");
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = static_cast<uint32_t>(want_w);
    fmt.fmt.pix.height = static_cast<uint32_t>(want_h);
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
      throw std::runtime_error(errnoText("VIDIOC_S_FMT"));
    }

    // S_FMT 是「协商」而不是「设定」：驱动完全可以返回另一个分辨率或格式。
    // 旧版继续拿宏 WIDTH/HEIGHT 去算，一旦驱动调整过就会按错误尺寸读 buffer，
    // 轻则画面错位，重则越界。这里一律采用驱动返回的实际值。
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
      throw std::runtime_error(
          "摄像头不支持 YUYV(驱动改成了其他格式)。"
          "本程序目前只处理 YUYV，若设备只出 MJPEG 需另加解码分支");
    }
    width_ = static_cast<int>(fmt.fmt.pix.width);
    height_ = static_cast<int>(fmt.fmt.pix.height);
    // 每行实际字节数可能大于 width*2(驱动会做对齐填充)，必须用 bytesperline，
    // 否则每一行都会错开几个字节，画面呈现出斜切效果。
    stride_ = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline
                                       : static_cast<size_t>(width_) * 2;

    v4l2_requestbuffers req{};
    req.count = static_cast<uint32_t>(want_buffers);
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
      throw std::runtime_error(errnoText("VIDIOC_REQBUFS"));
    }
    if (req.count < 2) {
      throw std::runtime_error("驱动可用缓冲区不足(少于 2 个)，无法稳定采集");
    }

    // 关键点：buffer_count_ 只在每次 mmap 真正成功之后才递增。
    // 旧版在申请之前就把数量写成 4，于是 REQBUFS 失败时析构照样按 4 遍历，
    // 直接越界读一个空 vector。
    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
        throw std::runtime_error(errnoText("VIDIOC_QUERYBUF"));
      }

      void* p = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
      if (p == MAP_FAILED) throw std::runtime_error(errnoText("mmap"));
      buffers_[i].start = p;
      buffers_[i].length = buf.length;
      buffer_count_ = i + 1;

      if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        throw std::runtime_error(errnoText("VIDIOC_QBUF"));
      }
    }
  } catch (...) {
    closeDevice();
    throw;
  }
}

Camera::~Camera() { closeDevice(); }

void Camera::closeDevice() {
  if (streaming_) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;
  }
  // 只回收真正映射成功的那几个，长度也用当时记录的实际长度。
  for (uint32_t i = 0; i < buffer_count_; ++i) {
    if (buffers_[i].start && buffers_[i].start != MAP_FAILED) {
      munmap(buffers_[i].start, buffers_[i].length);
    }
  }
  buffers_.clear();
  buffer_count_ = 0;
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

bool Camera::waitFrame(int timeout_ms) const {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd_, &fds);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  int r;
  do {
    r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
  } while (r == -1 && errno == EINTR);
  return r > 0;
}

std::vector<uint8_t> Camera::frameToJpeg(const uint8_t* yuyv) const {
  std::vector<uint8_t> rgb(static_cast<size_t>(width_) * height_ * 3);
  yuyvToRgb24(yuyv, stride_, rgb.data(), width_, height_);

  jpeg_compress_struct cinfo{};
  // 在 setjmp 之前建好，之后这个指针再也不变——这是上面那条规则要求的。
  // 析构时会自动 free 掉 libjpeg 分配的缓冲，成功和失败两条路都覆盖到。
  const auto session = std::make_unique<JpegSession>();
  cinfo.err = jpeg_std_error(&session->pub);
  session->pub.error_exit = jpegErrorExit;

  if (setjmp(session->jump)) {
    // libjpeg 报错会跳到这里。缓冲由 session 的析构负回收。
    jpeg_destroy_compress(&cinfo);
    throw std::runtime_error(std::string("JPEG 压缩失败: ") + session->message);
  }

  jpeg_create_compress(&cinfo);
  // 直接用 libjpeg 自带的内存输出。旧版手写了一整套 destination manager
  // (init/empty/term 三个回调 + 倍增扩容)，逻辑其实写对了，
  // 但那是 libjpeg 8 之前才需要干的活，现在一个标准函数就够，
  // 省掉 40 行最容易写错的指针算术。
  jpeg_mem_dest(&cinfo, &session->data, &session->size);

  cinfo.image_width = static_cast<JDIMENSION>(width_);
  cinfo.image_height = static_cast<JDIMENSION>(height_);
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, jpeg_quality_, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  const size_t row_stride = static_cast<size_t>(width_) * 3;
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = rgb.data() + cinfo.next_scanline * row_stride;
    jpeg_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  std::vector<uint8_t> jpeg(session->data, session->data + session->size);
  jpeg_destroy_compress(&cinfo);
  return jpeg;  // session 析构时 free 掉 libjpeg 的缓冲
}

std::vector<std::string> Camera::captureBurst() {
  std::vector<std::string> images;

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    throw std::runtime_error(errnoText("VIDIOC_STREAMON"));
  }
  streaming_ = true;

  const long long start = nowMs();
  long long last_shot = -1;
  images.reserve(static_cast<size_t>(duration_ms_ / interval_ms_ + 1));

  while (nowMs() - start < duration_ms_) {
    // 超时给 2 秒：USB 摄像头刚开流时第一帧常要等几百毫秒。
    // 超时就收手，不死等——这样上层一定能拿到「完成」信号。
    if (!waitFrame(2000)) break;

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN) continue;  // 还没就绪，再等一轮
      break;
    }
    if (buf.index >= buffer_count_) break;  // 驱动给了越界索引，保险起见停手

    const long long now = nowMs();
    // 只在到点时才做转换和压缩。出队的帧远多于我们要留的帧(30fps 出帧、
    // 500ms 取一张)，其余立刻还回去，不把 CPU 花在不会用到的帧上。
    if (last_shot < 0 || now - last_shot >= interval_ms_) {
      try {
        images.push_back(base64Encode(
            frameToJpeg(static_cast<const uint8_t*>(buffers_[buf.index].start))));
        last_shot = now;
      } catch (const std::exception&) {
        // 单帧压缩失败不该让整次采集报废：还回 buffer，继续下一帧。
        xioctl(fd_, VIDIOC_QBUF, &buf);
        continue;
      }
    }

    // 不管这一帧用没用，buffer 都必须还给驱动，否则队列很快就空了。
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) break;
  }

  xioctl(fd_, VIDIOC_STREAMOFF, &type);
  streaming_ = false;
  return images;
}

}  // namespace ar
