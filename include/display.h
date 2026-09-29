#ifndef AR_DISPLAY_H
#define AR_DISPLAY_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "config.h"

namespace ar {

// 无窗口系统的文字显示：DRM/KMS + GBM + EGL 直接画到扫描输出缓冲区，
// 不依赖 X11 / Wayland,适合嵌入式 AR 设备。
//
// 这个文件相比旧版改动最大，因为旧版有四个各自独立的硬伤：
//
// 1) connector/crtc/plane 的编号是写死的常量(208/71/57)，
//    注释里写着「我已经在测试阶段确认四个参数所以直接使用」。
//    换一块屏、换一个内核版本，这三个数字就全变了，表现是黑屏或直接报错。
//    现在改成启动时枚举 DRM 资源自动发现。
//
// 2) 二十多处失败路径直接 exit(EXIT_FAILURE)，一个显示函数有权杀掉整个进程，
//    而且已经申请的 DRM/GBM/EGL 资源全都不释放。现在统一抛异常。
//
// 3) 每个字形每次绘制都 glGenTextures + glTexImage2D + glDeleteTextures。
//    一屏两百字就是两百次纹理创建销毁，而且同一个字反复出现也不复用。
//    现在加了字形缓存。
//
// 4) 没有自动换行，AI 回复超过屏宽就直接画到屏幕外面去了。
//    对一个「AR 信息叠加」项目来说这是功能缺失，不是小瑕疵。
class DrmDisplay {
 public:
  explicit DrmDisplay(const Config& cfg);
  ~DrmDisplay();

  DrmDisplay(const DrmDisplay&) = delete;
  DrmDisplay& operator=(const DrmDisplay&) = delete;

  // 把一段 UTF-8 文本显示到屏幕上，自动换行。
  // 画完就返回，画面一直留在屏幕上直到下一次调用或对象析构，
  // 所以不需要旧版那个 sleep(5)。
  void showText(const std::string& utf8);

  // 清屏(纯背景色)。
  void clear();

  int width() const { return static_cast<int>(width_); }
  int height() const { return static_cast<int>(height_); }

 private:
  // 一个已经上传到 GPU 的字形。位图指标要一起存，
  // 否则下次用缓存时不知道该画多大、往哪偏移。
  struct Glyph {
    GLuint texture = 0;
    int width = 0;
    int height = 0;
    int bearing_x = 0;  // 从笔尖到位图左边的距离
    int bearing_y = 0;  // 从基线到位图顶边的距离
    float advance = 0;  // 画完这个字，笔尖往右移多少
  };

  // 初始化的四个阶段，拆开只是为了让构造函数读起来是条直线。
  void openDrmDevice(const Config& cfg);
  void findDisplayPath();
  void initGbmAndEgl();
  void initFreeType(const Config& cfg);
  void initGlProgram();

  void teardown();  // 按依赖倒序释放，析构和构造失败都走这里

  const Glyph& glyphFor(uint32_t codepoint);

  // 按可用宽度把码点序列切成若干行。
  // 中日韩文字可以在任意两字之间断，拉丁词要尽量在空格处断，
  // 混排的时候两套规则都得照顾到。
  std::vector<std::vector<uint32_t>> wrapText(const std::vector<uint32_t>& cps, float max_width);

  void drawGlyph(const Glyph& g, float pen_x, float pen_y);

  // 渲染完一帧后：交换缓冲、包成 DRM framebuffer、送去扫描输出。
  void present();

  int drm_fd_ = -1;
  uint32_t connector_id_ = 0;
  uint32_t crtc_id_ = 0;
  drmModeModeInfo mode_{};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  // 进入程序前的 CRTC 状态，退出时还回去，这样控制台能恢复，
  // 不会留下一块黑屏让用户以为机器挂了。
  drmModeCrtc* saved_crtc_ = nullptr;

  gbm_device* gbm_dev_ = nullptr;
  gbm_surface* gbm_surf_ = nullptr;

  EGLDisplay egl_dpy_ = EGL_NO_DISPLAY;
  EGLContext egl_ctx_ = EGL_NO_CONTEXT;
  EGLSurface egl_surf_ = EGL_NO_SURFACE;

  FT_Library ft_ = nullptr;
  FT_Face face_ = nullptr;
  int font_size_ = 0;
  float line_height_ = 0;
  float margin_x_ = 0;
  float margin_y_ = 0;

  GLuint program_ = 0;
  GLuint vbo_ = 0;
  GLint loc_pos_ = -1;
  GLint loc_uv_ = -1;
  GLint loc_viewport_ = -1;
  GLint loc_color_ = -1;
  GLint loc_tex_ = -1;

  std::unordered_map<uint32_t, Glyph> glyph_cache_;

  // 正在扫描输出的那一对 bo/fb。新的一帧上屏之后才能释放它们，
  // 提前释放会让显示控制器读到已经归还的显存。
  gbm_bo* front_bo_ = nullptr;
  uint32_t front_fb_ = 0;

  float bg_[3] = {0.f, 0.f, 0.f};
  float fg_[3] = {1.f, 1.f, 1.f};
};

}  // namespace ar

#endif  // AR_DISPLAY_H
