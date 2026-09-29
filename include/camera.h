#ifndef AR_CAMERA_H
#define AR_CAMERA_H

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace ar {

// V4L2 摄像头采集，输出一串 base64 编码的 JPEG。
//
// 相比旧版的三个全局函数(v4l2_init/v4l2_work/v4l2_free)，这里改成 RAII 类，
// 主要解决两个实际问题：
//   1. 旧版 init 中途失败会直接 return -1，既不 close(fd) 也不 munmap
//      已经映射好的 buffer，反复重试就是稳定泄漏。
//   2. 旧版 free 用 g_req.count 去遍历 buffers，而 count 是在 REQBUFS
//      之前就赋成 4 的，一旦 REQBUFS 失败就会越界读空 vector。
// 现在句柄和映射都挂在对象上，析构统一收，构造失败就抛异常且不留残留状态。
class Camera {
 public:
  explicit Camera(const Config& cfg);
  ~Camera();

  // 不可拷贝：里面握着 fd 和 mmap 指针，拷贝一份会导致双重释放。
  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;

  // 按配置的间隔和总时长采集若干帧，每帧转成 base64 的 JPEG 返回。
  // 采集失败(比如设备被拔掉)返回已经拿到的部分，由调用方决定够不够用。
  std::vector<std::string> captureBurst();

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  struct MappedBuffer {
    void* start = nullptr;
    size_t length = 0;
  };

  // 把一帧 YUYV 转成 RGB24，再压成 JPEG。
  std::vector<uint8_t> frameToJpeg(const uint8_t* yuyv) const;

  // 等一帧数据就绪，带超时。旧版直接阻塞在 DQBUF 上，
  // 摄像头不出帧的时候整个采集线程就卡死了，主线程的条件变量也永远等不到。
  // 返回 false 表示超时或出错。
  bool waitFrame(int timeout_ms) const;

  void closeDevice();

  std::string device_;
  int fd_ = -1;
  int width_ = 0;
  int height_ = 0;
  int interval_ms_ = 0;
  int duration_ms_ = 0;
  int jpeg_quality_ = 0;
  uint32_t buffer_count_ = 0;   // 实际映射成功的数量，不是申请的数量
  size_t stride_ = 0;           // 驱动报告的每行字节数，可能大于 width*2
  std::vector<MappedBuffer> buffers_;
  bool streaming_ = false;
};

}  // namespace ar

#endif  // AR_CAMERA_H
