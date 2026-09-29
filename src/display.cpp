#include "display.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <EGL/eglext.h>

namespace ar {
namespace {

std::string errText(const char* what) {
  return std::string(what) + " 失败: " + std::strerror(errno);
}

// 逐个解析 UTF-8 码点。
// 相比旧版多做了两件事：校验续字节是否真的是 10xxxxxx，以及检查剩余长度。
// 旧版不校验，字符串被截断在多字节字符中间时会一直往后读，属于越界读。
// AI 返回的文本是网络数据，按「可能是任意字节」来处理才安全。
std::vector<uint32_t> decodeUtf8(const std::string& s) {
  std::vector<uint32_t> out;
  out.reserve(s.size());
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    size_t need = 0;

    if (c < 0x80) {
      cp = c;
      need = 0;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      need = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      need = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      need = 3;
    } else {
      // 非法起始字节，跳过，用 U+FFFD 占位让用户看到「这里有问题」
      out.push_back(0xFFFD);
      ++i;
      continue;
    }

    // 需要读 i+1 .. i+need 这几个续字节，所以必须满足 i+need < n。
    // 不够就说明字符串在多字节字符中间被截断了——旧版不做这个检查，
    // 会一直往 string 末尾之外读，是实打实的越界。
    if (i + need >= n) {
      out.push_back(0xFFFD);
      break;
    }

    bool ok = true;
    for (size_t k = 1; k <= need; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) {  // 续字节必须是 10xxxxxx
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    out.push_back(cp);
    i += need + 1;
  }
  return out;
}

// 判断一个码点是否属于「可以在它前面断行」的宽字符区间。
// 中日韩文字没有词间空格，排版上允许任意断行；
// 如果不特殊处理，一整段中文会被当成一个超长单词，一行都放不下。
bool isCjk(uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF) ||    // 韩文字母
         (cp >= 0x2E80 && cp <= 0x9FFF) ||    // 部首扩展 ~ CJK 统一汉字
         (cp >= 0xAC00 && cp <= 0xD7AF) ||    // 韩文音节
         (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK 兼容汉字
         (cp >= 0xFF00 && cp <= 0xFF60) ||    // 全角标点和字母
         (cp >= 0x20000 && cp <= 0x2FA1F);    // CJK 扩展
}

// 不该出现在行首的标点(逗号、句号、右括号之类)。
// 断行时如果下一个字是这些，就把断点往前挪一个字。
bool isNoBreakBefore(uint32_t cp) {
  switch (cp) {
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
    case 0xFF09:  // ）
    case 0x300D:  // 」
    case 0x300F:  // 』
    case ')':
    case ']':
    case '}':
    case ',':
    case '.':
    case ':':
    case ';':
    case '!':
    case '?':
      return true;
    default:
      return false;
  }
}

// 常见的中文字体位置。写成候选列表而不是单个硬编码路径，
// 是因为不同发行版装的 Noto 路径完全不一样，
// 旧版写死一个路径，换个系统就是「Cannot load font」然后 exit。
const char* kFontCandidates[] = {
    "/usr/share/fonts/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",  // 无中文，最后兜底
};

}  // namespace

DrmDisplay::DrmDisplay(const Config& cfg) {
  // 任一阶段失败都要把前面已经拿到的资源还回去。
  try {
    openDrmDevice(cfg);
    findDisplayPath();
    initGbmAndEgl();
    initFreeType(cfg);
    initGlProgram();

    margin_x_ = static_cast<float>(cfg.getInt("display.margin_x", 40));
    margin_y_ = static_cast<float>(cfg.getInt("display.margin_y", 60));

    const int bg = cfg.getInt("display.bg_rgb", 0x000000);
    const int fg = cfg.getInt("display.fg_rgb", 0xFFFFFF);
    bg_[0] = ((bg >> 16) & 0xFF) / 255.0f;
    bg_[1] = ((bg >> 8) & 0xFF) / 255.0f;
    bg_[2] = (bg & 0xFF) / 255.0f;
    fg_[0] = ((fg >> 16) & 0xFF) / 255.0f;
    fg_[1] = ((fg >> 8) & 0xFF) / 255.0f;
    fg_[2] = (fg & 0xFF) / 255.0f;
  } catch (...) {
    teardown();
    throw;
  }
}

DrmDisplay::~DrmDisplay() { teardown(); }

void DrmDisplay::openDrmDevice(const Config& cfg) {
  const std::string card = cfg.getString("display.card", "/dev/dri/card0");
  drm_fd_ = open(card.c_str(), O_RDWR | O_CLOEXEC);
  if (drm_fd_ < 0) {
    throw std::runtime_error("打开 DRM 设备 " + card + " 失败: " + std::strerror(errno) +
                             "\n  提示: 需要对 /dev/dri/* 有读写权限"
                             "(把用户加进 video 组，或用 sudo 运行)");
  }
}

void DrmDisplay::findDisplayPath() {
  // 这是替换旧版三个魔法数字的核心逻辑：
  // 枚举 DRM 资源，找一个「已连接且有可用模式」的 connector，
  // 再顺着它的 encoder 找到能驱动它的 CRTC。
  drmModeRes* res = drmModeGetResources(drm_fd_);
  if (!res) throw std::runtime_error(errText("drmModeGetResources"));

  // 用一个小结构保证 res 在任何退出路径上都会被释放
  struct ResGuard {
    drmModeRes* r;
    ~ResGuard() { drmModeFreeResources(r); }
  } guard{res};

  for (int i = 0; i < res->count_connectors && connector_id_ == 0; ++i) {
    drmModeConnector* conn = drmModeGetConnector(drm_fd_, res->connectors[i]);
    if (!conn) continue;

    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      // 优先选显示器自己标记为 PREFERRED 的模式(通常是原生分辨率)，
      // 没有就退回第一个。
      const drmModeModeInfo* chosen = &conn->modes[0];
      for (int m = 0; m < conn->count_modes; ++m) {
        if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
          chosen = &conn->modes[m];
          break;
        }
      }

      // 找 CRTC：先试 connector 当前挂着的 encoder(通常已经是对的)，
      // 不行就遍历它支持的所有 encoder，看哪个 encoder 的 possible_crtcs
      // 位掩码里有可用的 CRTC。
      uint32_t crtc = 0;
      if (conn->encoder_id) {
        drmModeEncoder* enc = drmModeGetEncoder(drm_fd_, conn->encoder_id);
        if (enc) {
          if (enc->crtc_id) crtc = enc->crtc_id;
          drmModeFreeEncoder(enc);
        }
      }
      for (int e = 0; e < conn->count_encoders && crtc == 0; ++e) {
        drmModeEncoder* enc = drmModeGetEncoder(drm_fd_, conn->encoders[e]);
        if (!enc) continue;
        for (int c = 0; c < res->count_crtcs; ++c) {
          // possible_crtcs 的第 c 位为 1，表示这个 encoder 能接到第 c 个 CRTC
          if (enc->possible_crtcs & (1u << c)) {
            crtc = res->crtcs[c];
            break;
          }
        }
        drmModeFreeEncoder(enc);
      }

      if (crtc) {
        connector_id_ = conn->connector_id;
        crtc_id_ = crtc;
        mode_ = *chosen;
        width_ = mode_.hdisplay;
        height_ = mode_.vdisplay;
      }
    }
    drmModeFreeConnector(conn);
  }

  if (connector_id_ == 0) {
    throw std::runtime_error(
        "没找到已连接的显示器。\n"
        "  提示: 确认屏幕已插好；若在无头环境下测试，"
        "本模块需要真实的 KMS 输出，无法在纯 SSH 会话里出图");
  }

  // 记下原来的 CRTC 配置，析构时还原，免得程序退出后留一块黑屏。
  saved_crtc_ = drmModeGetCrtc(drm_fd_, crtc_id_);

  std::cout << "[显示] connector=" << connector_id_ << " crtc=" << crtc_id_ << " 模式="
            << width_ << "x" << height_ << "@" << mode_.vrefresh << "Hz" << std::endl;
}

void DrmDisplay::initGbmAndEgl() {
  gbm_dev_ = gbm_create_device(drm_fd_);
  if (!gbm_dev_) throw std::runtime_error("gbm_create_device 失败");

  gbm_surf_ = gbm_surface_create(gbm_dev_, width_, height_, GBM_FORMAT_XRGB8888,
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!gbm_surf_) throw std::runtime_error("gbm_surface_create 失败");

  // eglGetPlatformDisplayEXT 是 GBM 平台的正路；
  // 老驱动上没有这个扩展，才退回 eglGetDisplay。
  auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (getPlatformDisplay) {
    egl_dpy_ = getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_dev_, nullptr);
  }
  if (egl_dpy_ == EGL_NO_DISPLAY) {
    egl_dpy_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm_dev_));
  }
  if (egl_dpy_ == EGL_NO_DISPLAY) throw std::runtime_error("取不到 EGLDisplay");

  if (!eglInitialize(egl_dpy_, nullptr, nullptr)) {
    throw std::runtime_error("eglInitialize 失败");
  }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    throw std::runtime_error("eglBindAPI(ES) 失败");
  }

  const EGLint cfg_attribs[] = {EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                                EGL_RED_SIZE,        8,
                                EGL_GREEN_SIZE,      8,
                                EGL_BLUE_SIZE,       8,
                                EGL_ALPHA_SIZE,      0,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                EGL_NONE};

  // 不能随便拿第一个匹配的 config：它的原生可视格式必须和我们创建
  // gbm_surface 时用的 XRGB8888 一致，否则 eglCreateWindowSurface 会失败，
  // 或者上屏后颜色通道错位(红蓝互换那种)。旧版只取第一个并要求 ncfg==1,
  // 在多 config 的驱动上会直接报错退出。
  EGLint total = 0;
  if (!eglChooseConfig(egl_dpy_, cfg_attribs, nullptr, 0, &total) || total <= 0) {
    throw std::runtime_error("没有满足条件的 EGLConfig");
  }
  std::vector<EGLConfig> configs(static_cast<size_t>(total));
  eglChooseConfig(egl_dpy_, cfg_attribs, configs.data(), total, &total);

  EGLConfig chosen = nullptr;
  for (EGLint i = 0; i < total; ++i) {
    EGLint visual = 0;
    if (eglGetConfigAttrib(egl_dpy_, configs[i], EGL_NATIVE_VISUAL_ID, &visual) &&
        static_cast<uint32_t>(visual) == GBM_FORMAT_XRGB8888) {
      chosen = configs[i];
      break;
    }
  }
  if (!chosen) chosen = configs[0];  // 驱动没报 visual id，只能赌第一个

  const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  egl_ctx_ = eglCreateContext(egl_dpy_, chosen, EGL_NO_CONTEXT, ctx_attribs);
  if (egl_ctx_ == EGL_NO_CONTEXT) throw std::runtime_error("eglCreateContext 失败");

  egl_surf_ = eglCreateWindowSurface(
      egl_dpy_, chosen, reinterpret_cast<EGLNativeWindowType>(gbm_surf_), nullptr);
  if (egl_surf_ == EGL_NO_SURFACE) throw std::runtime_error("eglCreateWindowSurface 失败");

  if (!eglMakeCurrent(egl_dpy_, egl_surf_, egl_surf_, egl_ctx_)) {
    throw std::runtime_error("eglMakeCurrent 失败");
  }
  glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
}

void DrmDisplay::initFreeType(const Config& cfg) {
  if (FT_Init_FreeType(&ft_) != 0) throw std::runtime_error("FT_Init_FreeType 失败");

  font_size_ = cfg.getInt("display.font_size", 28);
  if (font_size_ < 8) throw std::runtime_error("display.font_size 太小(至少 8)");

  // 配置里指定了就只用它(指定了却加载不上应该明确报错，
  // 而不是悄悄换成别的字体让用户以为配置生效了)；没指定才走候选列表。
  const std::string configured = cfg.getString("display.font_path", "");
  if (!configured.empty()) {
    if (FT_New_Face(ft_, configured.c_str(), 0, &face_) != 0) {
      throw std::runtime_error("加载字体失败: " + configured);
    }
  } else {
    for (const char* path : kFontCandidates) {
      if (FT_New_Face(ft_, path, 0, &face_) == 0) {
        std::cout << "[显示] 使用字体 " << path << std::endl;
        break;
      }
      face_ = nullptr;
    }
    if (!face_) {
      throw std::runtime_error(
          "找不到可用字体。\n"
          "  提示: 安装中文字体(Debian/Ubuntu: apt install fonts-noto-cjk)，"
          "或用 display.font_path 指定一个 .ttf/.ttc 文件");
    }
  }

  FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(font_size_));
  // metrics.height 是 26.6 定点数(低 6 位是小数)，右移 6 位取整数像素
  line_height_ = static_cast<float>(face_->size->metrics.height >> 6);
  if (line_height_ <= 0) line_height_ = static_cast<float>(font_size_) * 1.3f;
}

void DrmDisplay::initGlProgram() {
  // 顶点着色器把像素坐标转成 NDC。
  // 这样上层可以直接用「左上角为原点、向下为正」的屏幕坐标思考，
  // 不用每次都手算 -1..1 的映射。
  static const char* kVs = R"(
attribute vec2 aPos;
attribute vec2 aUv;
varying vec2 vUv;
uniform vec2 uViewport;
void main() {
  vec2 ndc = (aPos / uViewport) * 2.0 - 1.0;
  gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
  vUv = aUv;
})";

  // 字形纹理只有一个通道(FreeType 给的是 8 位灰度覆盖率)，
  // 当成 alpha 用，颜色由 uniform 给。旧版把灰度复制成 RGBA 四个通道
  // 再上传，显存和带宽都是 4 倍浪费。
  static const char* kFs = R"(
precision mediump float;
varying vec2 vUv;
uniform sampler2D uTex;
uniform vec3 uColor;
void main() {
  float a = texture2D(uTex, vUv).a;
  gl_FragColor = vec4(uColor, a);
})";

  auto compile = [](GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512] = {0};
      glGetShaderInfoLog(s, sizeof(log), nullptr, log);
      glDeleteShader(s);
      throw std::runtime_error(std::string("着色器编译失败: ") + log);
    }
    return s;
  };

  GLuint vs = compile(GL_VERTEX_SHADER, kVs);
  GLuint fs = 0;
  try {
    fs = compile(GL_FRAGMENT_SHADER, kFs);
  } catch (...) {
    glDeleteShader(vs);
    throw;
  }

  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  // shader 对象在 link 之后就可以删了，program 会保留需要的部分
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint ok = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512] = {0};
    glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
    throw std::runtime_error(std::string("着色器链接失败: ") + log);
  }

  loc_pos_ = glGetAttribLocation(program_, "aPos");
  loc_uv_ = glGetAttribLocation(program_, "aUv");
  loc_viewport_ = glGetUniformLocation(program_, "uViewport");
  loc_color_ = glGetUniformLocation(program_, "uColor");
  loc_tex_ = glGetUniformLocation(program_, "uTex");

  glGenBuffers(1, &vbo_);
}

const DrmDisplay::Glyph& DrmDisplay::glyphFor(uint32_t cp) {
  auto it = glyph_cache_.find(cp);
  if (it != glyph_cache_.end()) return it->second;

  Glyph g;
  if (FT_Load_Char(face_, cp, FT_LOAD_RENDER) == 0) {
    FT_GlyphSlot slot = face_->glyph;
    FT_Bitmap& bmp = slot->bitmap;
    g.width = static_cast<int>(bmp.width);
    g.height = static_cast<int>(bmp.rows);
    g.bearing_x = slot->bitmap_left;
    g.bearing_y = slot->bitmap_top;
    g.advance = static_cast<float>(slot->advance.x >> 6);  // 26.6 定点转像素

    if (g.width > 0 && g.height > 0) {
      // FreeType 的 bitmap 每行可能有 padding(pitch != width)，
      // 而 glTexImage2D 要求紧凑排列，所以逐行拷一遍。
      std::vector<uint8_t> tight(static_cast<size_t>(g.width) * g.height);
      for (int row = 0; row < g.height; ++row) {
        std::memcpy(tight.data() + static_cast<size_t>(row) * g.width,
                    bmp.buffer + static_cast<size_t>(row) * bmp.pitch,
                    static_cast<size_t>(g.width));
      }

      glGenTextures(1, &g.texture);
      glBindTexture(GL_TEXTURE_2D, g.texture);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);  // 单通道数据不是 4 字节对齐的
      glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, g.width, g.height, 0, GL_ALPHA,
                   GL_UNSIGNED_BYTE, tight.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      // 不加 CLAMP_TO_EDGE 的话，线性采样会在字形边缘取到对侧像素，
      // 表现为字的四周有一圈淡淡的重影。
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
  } else {
    // 字体里没有这个字：给一个空槽(宽度按半个字号)，至少不会卡住排版
    g.advance = static_cast<float>(font_size_) * 0.5f;
  }

  return glyph_cache_.emplace(cp, g).first->second;
}

std::vector<std::vector<uint32_t>> DrmDisplay::wrapText(const std::vector<uint32_t>& cps,
                                                        float max_width) {
  std::vector<std::vector<uint32_t>> lines;
  std::vector<uint32_t> line;
  float x = 0;
  // 最近一个「适合断行」的位置(拉丁文的空格后面)，以及那里的宽度
  size_t last_break = std::string::npos;

  for (size_t i = 0; i < cps.size(); ++i) {
    const uint32_t cp = cps[i];

    if (cp == '\n') {  // 原文里的换行照原样断
      lines.push_back(line);
      line.clear();
      x = 0;
      last_break = std::string::npos;
      continue;
    }

    const Glyph& g = glyphFor(cp);

    if (x + g.advance > max_width && !line.empty()) {
      if (isCjk(cp) || last_break == std::string::npos) {
        // 中日韩文字可以在这里直接断；拉丁文找不到空格(比如一串很长的
        // URL)也只能硬断，总比画出屏幕外面好。
        // 唯一要避让的是标点不能顶在行首。
        if (isNoBreakBefore(cp) && line.size() > 1) {
          // 把上一个字一起挪到下一行，让标点跟着它走
          const uint32_t moved = line.back();
          line.pop_back();
          lines.push_back(line);
          line.clear();
          line.push_back(moved);
          x = glyphFor(moved).advance;
        } else {
          lines.push_back(line);
          line.clear();
          x = 0;
        }
      } else {
        // 拉丁文：回退到最后一个空格处断行，把那之后的字符搬到下一行，
        // 这样单词不会被切成两半。
        std::vector<uint32_t> carry(line.begin() + static_cast<long>(last_break) + 1,
                                    line.end());
        line.resize(last_break);  // 丢掉行尾那个空格
        lines.push_back(line);
        line = carry;
        x = 0;
        for (uint32_t c : line) x += glyphFor(c).advance;
      }
      last_break = std::string::npos;
    }

    line.push_back(cp);
    x += g.advance;
    if (cp == ' ' || cp == '\t') last_break = line.size() - 1;
  }

  if (!line.empty()) lines.push_back(line);
  return lines;
}

void DrmDisplay::drawGlyph(const Glyph& g, float pen_x, float pen_y) {
  if (g.texture == 0) return;  // 空格之类没有位图的字形

  const float x0 = pen_x + static_cast<float>(g.bearing_x);
  const float y0 = pen_y - static_cast<float>(g.bearing_y);
  const float x1 = x0 + static_cast<float>(g.width);
  const float y1 = y0 + static_cast<float>(g.height);

  // 两个三角形拼一个矩形，(x,y,u,v) 交错存放
  const float verts[] = {
      x0, y0, 0.f, 0.f,  x1, y0, 1.f, 0.f,  x1, y1, 1.f, 1.f,
      x0, y0, 0.f, 0.f,  x1, y1, 1.f, 1.f,  x0, y1, 0.f, 1.f,
  };

  glBindTexture(GL_TEXTURE_2D, g.texture);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
  glVertexAttribPointer(loc_pos_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        reinterpret_cast<void*>(0));
  glVertexAttribPointer(loc_uv_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        reinterpret_cast<void*>(2 * sizeof(float)));
  glDrawArrays(GL_TRIANGLES, 0, 6);
}

void DrmDisplay::clear() {
  glClearColor(bg_[0], bg_[1], bg_[2], 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  present();
}

void DrmDisplay::showText(const std::string& utf8) {
  glClearColor(bg_[0], bg_[1], bg_[2], 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(program_);
  glUniform2f(loc_viewport_, static_cast<float>(width_), static_cast<float>(height_));
  glUniform3f(loc_color_, fg_[0], fg_[1], fg_[2]);
  glUniform1i(loc_tex_, 0);
  glActiveTexture(GL_TEXTURE0);

  // 字形是带 alpha 的覆盖率图，必须开混合，否则字的边缘会是硬锯齿，
  // 而且矩形的透明部分会画成背景色块。
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glEnableVertexAttribArray(loc_pos_);
  glEnableVertexAttribArray(loc_uv_);

  const float usable_w = static_cast<float>(width_) - 2 * margin_x_;
  auto lines = wrapText(decodeUtf8(utf8), usable_w);

  // 一屏放不下就截断，并且明确告诉用户被截了——
  // 默默丢内容比截断更糟，用户会以为 AI 就回答了这么多。
  const int max_lines =
      static_cast<int>((static_cast<float>(height_) - 2 * margin_y_) / line_height_);
  bool truncated = false;
  if (max_lines > 1 && static_cast<int>(lines.size()) > max_lines) {
    lines.resize(static_cast<size_t>(max_lines) - 1);
    truncated = true;
  }

  float pen_y = margin_y_ + line_height_;
  for (const auto& line : lines) {
    float pen_x = margin_x_;
    for (uint32_t cp : line) {
      const Glyph& g = glyphFor(cp);
      drawGlyph(g, pen_x, pen_y);
      pen_x += g.advance;
    }
    pen_y += line_height_;
  }

  if (truncated) {
    float pen_x = margin_x_;
    for (uint32_t cp : decodeUtf8("...(内容过长，已截断)")) {
      const Glyph& g = glyphFor(cp);
      drawGlyph(g, pen_x, pen_y);
      pen_x += g.advance;
    }
  }

  glDisableVertexAttribArray(loc_pos_);
  glDisableVertexAttribArray(loc_uv_);
  present();
}

void DrmDisplay::present() {
  if (!eglSwapBuffers(egl_dpy_, egl_surf_)) {
    throw std::runtime_error("eglSwapBuffers 失败");
  }

  gbm_bo* bo = gbm_surface_lock_front_buffer(gbm_surf_);
  if (!bo) throw std::runtime_error("gbm_surface_lock_front_buffer 失败");

  const uint32_t handle = gbm_bo_get_handle(bo).u32;
  const uint32_t stride = gbm_bo_get_stride(bo);
  uint32_t handles[4] = {handle, 0, 0, 0};
  uint32_t strides[4] = {stride, 0, 0, 0};
  uint32_t offsets[4] = {0, 0, 0, 0};

  uint32_t fb = 0;
  if (drmModeAddFB2(drm_fd_, width_, height_, GBM_FORMAT_XRGB8888, handles, strides,
                    offsets, &fb, 0) != 0) {
    gbm_surface_release_buffer(gbm_surf_, bo);
    throw std::runtime_error(errText("drmModeAddFB2"));
  }

  // 用 SetCrtc 走标准 modeset 路径，而不是旧版的 drmModeSetPlane。
  // SetPlane 需要事先知道一个能用的 overlay plane id(旧版就是硬编码 57)，
  // 而 SetCrtc 只要 connector 和 crtc，这两个我们已经自动发现了，
  // 可移植性完全不同。
  if (drmModeSetCrtc(drm_fd_, crtc_id_, fb, 0, 0, &connector_id_, 1, &mode_) != 0) {
    drmModeRmFB(drm_fd_, fb);
    gbm_surface_release_buffer(gbm_surf_, bo);
    throw std::runtime_error(errText("drmModeSetCrtc"));
  }

  // 新的一帧已经在扫描输出了，这时候才能释放上一帧。
  // 顺序反过来的话，显示控制器会有一瞬间读到已经归还的显存。
  if (front_fb_) drmModeRmFB(drm_fd_, front_fb_);
  if (front_bo_) gbm_surface_release_buffer(gbm_surf_, front_bo_);
  front_fb_ = fb;
  front_bo_ = bo;
}

void DrmDisplay::teardown() {
  // 严格按依赖倒序释放。这个函数会被析构和构造失败两条路径调用，
  // 所以每一步都得先判空——构造到一半失败时，后面的成员还是初值。
  if (egl_dpy_ != EGL_NO_DISPLAY) {
    // 释放字形纹理必须在销毁 context 之前，context 没了 GL 调用就无效了
    if (egl_ctx_ != EGL_NO_CONTEXT) {
      eglMakeCurrent(egl_dpy_, egl_surf_, egl_surf_, egl_ctx_);
      for (auto& kv : glyph_cache_) {
        if (kv.second.texture) glDeleteTextures(1, &kv.second.texture);
      }
      glyph_cache_.clear();
      if (vbo_) glDeleteBuffers(1, &vbo_);
      if (program_) glDeleteProgram(program_);
    }
  }

  if (face_) {
    FT_Done_Face(face_);
    face_ = nullptr;
  }
  if (ft_) {
    FT_Done_FreeType(ft_);
    ft_ = nullptr;
  }

  // 还原进程启动前的画面，让控制台回来
  if (saved_crtc_) {
    if (saved_crtc_->mode_valid) {
      drmModeSetCrtc(drm_fd_, saved_crtc_->crtc_id, saved_crtc_->buffer_id, saved_crtc_->x,
                     saved_crtc_->y, &connector_id_, 1, &saved_crtc_->mode);
    }
    drmModeFreeCrtc(saved_crtc_);
    saved_crtc_ = nullptr;
  }

  if (front_fb_) {
    drmModeRmFB(drm_fd_, front_fb_);
    front_fb_ = 0;
  }
  if (front_bo_) {
    gbm_surface_release_buffer(gbm_surf_, front_bo_);
    front_bo_ = nullptr;
  }

  if (egl_dpy_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(egl_dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surf_ != EGL_NO_SURFACE) eglDestroySurface(egl_dpy_, egl_surf_);
    if (egl_ctx_ != EGL_NO_CONTEXT) eglDestroyContext(egl_dpy_, egl_ctx_);
    eglTerminate(egl_dpy_);
    egl_dpy_ = EGL_NO_DISPLAY;
    egl_surf_ = EGL_NO_SURFACE;
    egl_ctx_ = EGL_NO_CONTEXT;
  }

  if (gbm_surf_) {
    gbm_surface_destroy(gbm_surf_);
    gbm_surf_ = nullptr;
  }
  if (gbm_dev_) {
    gbm_device_destroy(gbm_dev_);
    gbm_dev_ = nullptr;
  }
  if (drm_fd_ >= 0) {
    close(drm_fd_);
    drm_fd_ = -1;
  }
}

}  // namespace ar
