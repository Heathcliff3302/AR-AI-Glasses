#include "audio.h"

#include <alsa/asoundlib.h>
#include <whisper.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace ar {
namespace {

constexpr unsigned int kWhisperRate = 16000;  // whisper 只吃 16kHz 单声道

// 自己定义而不是用 M_PI。M_PI 来自 POSIX 而非 ISO C++，
// 在 -std=c++17（本项目设了 CMAKE_CXX_EXTENSIONS OFF，用的就是严格模式）下
// glibc 会因为 __STRICT_ANSI__ 而不导出它，构建会直接失败。
constexpr double kPi = 3.14159265358979323846;

// 用 unique_ptr 式的小包装管 snd_pcm_t。
// ALSA 的 C 接口有十几个可能失败的点，手写 close 很容易在某个
// 早退分支上漏掉，句柄泄漏的表现是「第二次录音打不开设备」。
class PcmHandle {
 public:
  explicit PcmHandle(snd_pcm_t* h) : h_(h) {}
  ~PcmHandle() {
    if (h_) snd_pcm_close(h_);
  }
  PcmHandle(const PcmHandle&) = delete;
  PcmHandle& operator=(const PcmHandle&) = delete;
  snd_pcm_t* get() const { return h_; }

 private:
  snd_pcm_t* h_;
};

// 去掉首尾空白。whisper 每个 segment 的文本通常带一个前导空格。
std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

}  // namespace

SpeechToText::SpeechToText(const Config& cfg) {
  device_ = cfg.getString("audio.device", "default");
  duration_sec_ = cfg.getInt("audio.duration_sec", 10);
  language_ = cfg.getString("whisper.language", "zh");
  // 默认用一半核心数：另一半留给同时在跑的图像采集和 JPEG 压缩线程，
  // 全占满反而会因为互相抢 CPU 而更慢。
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  n_threads_ = cfg.getInt("whisper.threads", static_cast<int>(std::max(1u, hw / 2)));

  if (duration_sec_ <= 0) throw std::runtime_error("audio.duration_sec 必须大于 0");

  const std::string model_path = cfg.requireString("whisper.model_path");

  whisper_context_params cparams = whisper_context_default_params();
  cparams.use_gpu = cfg.getBool("whisper.use_gpu", false);

  ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
  if (!ctx_) {
    throw std::runtime_error(
        "加载 whisper 模型失败: " + model_path +
        "\n  提示: 确认路径正确且文件完整。模型可用 whisper.cpp 自带脚本下载:"
        "\n        bash models/download-ggml-model.sh base");
  }
}

SpeechToText::~SpeechToText() {
  if (ctx_) whisper_free(ctx_);
}

std::vector<float> SpeechToText::recordPcm(unsigned int* actual_rate) const {
  snd_pcm_t* raw = nullptr;
  int rc = snd_pcm_open(&raw, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    throw std::runtime_error("打开录音设备 " + device_ + " 失败: " + snd_strerror(rc) +
                             "\n  提示: 用 arecord -l 查看可用设备，"
                             "配置成 default 或 hw:<卡号>,<设备号>");
  }
  PcmHandle pcm(raw);

  snd_pcm_hw_params_t* params = nullptr;
  snd_pcm_hw_params_alloca(&params);
  snd_pcm_hw_params_any(pcm.get(), params);

  // 这几项失败就没法继续了，逐个检查返回值。
  // 旧版只检查了最后的 snd_pcm_hw_params，中间四个 set_* 全不看返回值，
  // 于是「设备不支持单声道」这类问题会一路滑到后面才以怪异的形式暴露。
  if ((rc = snd_pcm_hw_params_set_access(pcm.get(), params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
    throw std::runtime_error(std::string("设置访问模式失败: ") + snd_strerror(rc));
  }
  if ((rc = snd_pcm_hw_params_set_format(pcm.get(), params, SND_PCM_FORMAT_S16_LE)) < 0) {
    throw std::runtime_error(std::string("设置采样格式 S16_LE 失败: ") + snd_strerror(rc));
  }
  if ((rc = snd_pcm_hw_params_set_channels(pcm.get(), params, 1)) < 0) {
    throw std::runtime_error(std::string("设置单声道失败: ") + snd_strerror(rc) +
                             "\n  提示: 若设备只支持立体声，需要改成采集 2 声道后取一路");
  }

  // 这是修掉混叠问题的关键一步：直接向硬件要 16kHz。
  // set_rate_near 会把 rate 改写成硬件实际能给的最接近值，
  // 所以下面必须回读它，不能假设拿到的就是 16000。
  unsigned int rate = kWhisperRate;
  if ((rc = snd_pcm_hw_params_set_rate_near(pcm.get(), params, &rate, nullptr)) < 0) {
    throw std::runtime_error(std::string("设置采样率失败: ") + snd_strerror(rc));
  }

  if ((rc = snd_pcm_hw_params(pcm.get(), params)) < 0) {
    throw std::runtime_error(std::string("应用硬件参数失败: ") + snd_strerror(rc));
  }

  snd_pcm_uframes_t period = 0;
  if (snd_pcm_hw_params_get_period_size(params, &period, nullptr) < 0 || period == 0) {
    period = 1024;  // 取不到就用一个常见值兜底
  }

  if ((rc = snd_pcm_prepare(pcm.get())) < 0) {
    throw std::runtime_error(std::string("准备录音失败: ") + snd_strerror(rc));
  }

  *actual_rate = rate;
  const size_t target = static_cast<size_t>(rate) * duration_sec_;

  std::vector<int16_t> buf(period);
  std::vector<float> samples;
  samples.reserve(target);

  std::cout << "[音频] 录音 " << duration_sec_ << " 秒 (采样率 " << rate << " Hz)..."
            << std::endl;

  int consecutive_errors = 0;
  while (samples.size() < target) {
    const snd_pcm_sframes_t frames = snd_pcm_readi(pcm.get(), buf.data(), period);
    if (frames < 0) {
      // overrun / 设备临时不可用都可以恢复，恢复不了才是真的完了。
      if (snd_pcm_recover(pcm.get(), static_cast<int>(frames), 1) < 0) {
        throw std::runtime_error(std::string("读取音频失败: ") +
                                 snd_strerror(static_cast<int>(frames)));
      }
      // 能恢复也要防着它一直恢复一直失败，否则这里会变成死循环。
      if (++consecutive_errors > 10) {
        throw std::runtime_error("音频设备反复出错，放弃录音");
      }
      continue;
    }
    consecutive_errors = 0;

    // int16 转 [-1,1] 浮点。除数用 32768 而不是 32767：
    // int16 的范围是 -32768..32767，用 32768 才不会让最负的样本溢出到 -1 以下。
    const size_t take = std::min(static_cast<size_t>(frames), target - samples.size());
    for (size_t i = 0; i < take; ++i) {
      samples.push_back(static_cast<float>(buf[i]) / 32768.0f);
    }
  }

  snd_pcm_drop(pcm.get());
  return samples;
}

std::vector<float> SpeechToText::resampleTo16k(const std::vector<float>& in, unsigned int in_rate) {
  if (in_rate == kWhisperRate || in.empty()) return in;
  if (in_rate < kWhisperRate) {
    // 升采样场景(设备只能给 8kHz 之类)。信息已经丢了，补不回来，
    // 直接线性插值填到 16k，让 whisper 至少能处理。
    const double ratio = static_cast<double>(in_rate) / kWhisperRate;
    const size_t out_n = static_cast<size_t>(in.size() / ratio);
    std::vector<float> out(out_n);
    for (size_t i = 0; i < out_n; ++i) {
      const double pos = i * ratio;
      const size_t i0 = static_cast<size_t>(pos);
      const size_t i1 = std::min(i0 + 1, in.size() - 1);
      const double frac = pos - i0;
      out[i] = static_cast<float>(in[i0] * (1.0 - frac) + in[i1] * frac);
    }
    return out;
  }

  // 降采样：必须先低通再抽取，顺序反了就是旧版那个 bug。
  //
  // 原理：目标 16kHz 只能无歧义地表示 8kHz 以下的频率(奈奎斯特定律)。
  // 源信号里高于 8kHz 的成分如果不先滤掉，抽取之后会"折返"成 8kHz 以下的
  // 假信号叠在语音上——听起来像金属噪声，对识别率的影响相当直接。
  //
  // 滤波器用窗函数法设计的 FIR：理想低通的时域响应是 sinc 函数，
  // 截断它会产生振铃，所以乘一个 Hamming 窗压住旁瓣。
  // 截止频率取 7.2kHz(0.45 × 16k)，留一点过渡带余量。
  const int kTaps = 101;  // 奇数，保证有对称中心，群延迟是整数个样本
  const double cutoff_hz = 0.45 * kWhisperRate;
  const double fc = cutoff_hz / in_rate;  // 归一化截止频率(相对源采样率)

  std::vector<double> h(kTaps);
  const int mid = kTaps / 2;
  double sum = 0.0;
  for (int i = 0; i < kTaps; ++i) {
    const int n = i - mid;
    // sinc(2*fc*n)，n=0 处取极限值 2*fc
    const double sinc = (n == 0) ? 2.0 * fc
                                 : std::sin(2.0 * kPi * fc * n) / (kPi * n);
    const double window = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (kTaps - 1));
    h[i] = sinc * window;
    sum += h[i];
  }
  // 归一化成直流增益 1，否则整体音量会跟着滤波器系数漂移。
  for (double& c : h) c /= sum;

  // 只在需要输出的位置上做卷积——反正大部分样本要被丢掉，
  // 没必要先把整条信号滤一遍再抽取，这样能省掉约 (in_rate/16000 - 1) 的计算量。
  const double step = static_cast<double>(in_rate) / kWhisperRate;
  const size_t out_n = static_cast<size_t>(in.size() / step);
  std::vector<float> out;
  out.reserve(out_n);

  const long n_in = static_cast<long>(in.size());
  for (size_t i = 0; i < out_n; ++i) {
    const long center = static_cast<long>(i * step);
    double acc = 0.0;
    for (int k = 0; k < kTaps; ++k) {
      const long idx = center + (k - mid);
      // 边界外按 0 处理(零填充)，只影响首尾各 50 个样本，约 3 毫秒，无关紧要。
      if (idx >= 0 && idx < n_in) acc += in[static_cast<size_t>(idx)] * h[k];
    }
    out.push_back(static_cast<float>(acc));
  }
  return out;
}

std::string SpeechToText::recordAndTranscribe() {
  unsigned int rate = 0;
  std::vector<float> pcm = recordPcm(&rate);
  if (pcm.empty()) return "";

  if (rate != kWhisperRate) {
    std::cout << "[音频] 设备只能提供 " << rate << " Hz，重采样到 " << kWhisperRate
              << " Hz(含抗混叠滤波)" << std::endl;
    pcm = resampleTo16k(pcm, rate);
  }
  if (pcm.empty()) return "";

  whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  wparams.print_progress = false;
  wparams.print_special = false;
  wparams.print_realtime = false;
  wparams.print_timestamps = false;
  wparams.translate = false;
  wparams.language = language_.c_str();
  wparams.n_threads = n_threads_;

  if (whisper_full(ctx_, wparams, pcm.data(), static_cast<int>(pcm.size())) != 0) {
    throw std::runtime_error("whisper 推理失败");
  }

  std::string text;
  const int n = whisper_full_n_segments(ctx_);
  for (int i = 0; i < n; ++i) {
    const char* seg = whisper_full_get_segment_text(ctx_, i);
    if (seg) text += seg;
  }
  return trim(text);
}

}  // namespace ar
