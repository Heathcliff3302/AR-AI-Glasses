#ifndef AR_AUDIO_H
#define AR_AUDIO_H

#include <string>
#include <vector>

#include "config.h"

// 前向声明，避免把 whisper.h 泄漏给所有包含者
struct whisper_context;

namespace ar {

// 麦克风录音 + 本地 whisper 语音转文字。
//
// 相比旧版的两处实质性改动：
//
// 1) 模型只加载一次。旧版每次录音都 whisper_init_from_file 再 whisper_free，
//    base 模型 140MB 左右，每次白等好几秒，纯属浪费。模型是无状态的，
//    完全可以常驻，所以放到构造函数里加载、析构里释放。
//
// 2) 采样率优先让 ALSA 直接给 16kHz。旧版固定按 48kHz 采集，然后
//    「每 3 个点取 1 个」降到 16kHz——这是没有抗混叠滤波的抽取，
//    8kHz 以上的成分会原封不动地折回可听频段变成噪声，直接拉低识别率。
//    现在先尝试让硬件出 16kHz(whisper 要的就是这个)；万一设备不支持，
//    才退回到「先低通滤波再重采样」的正确做法。
class SpeechToText {
 public:
  explicit SpeechToText(const Config& cfg);
  ~SpeechToText();

  // 持有 whisper 上下文指针，拷贝会导致重复 free
  SpeechToText(const SpeechToText&) = delete;
  SpeechToText& operator=(const SpeechToText&) = delete;

  // 录制配置时长的音频并转成文字。识别不到内容就返回空串
  // (说明用户没说话，不算错误，由调用方决定要不要继续)。
  std::string recordAndTranscribe();

 private:
  // 从 ALSA 采集单声道 PCM，返回 [-1,1] 的浮点样本和实际采样率。
  std::vector<float> recordPcm(unsigned int* actual_rate) const;

  // 把任意采样率的单声道音频转成 whisper 要的 16kHz。
  static std::vector<float> resampleTo16k(const std::vector<float>& in, unsigned int in_rate);

  std::string device_;
  int duration_sec_ = 0;
  std::string language_;
  int n_threads_ = 0;
  whisper_context* ctx_ = nullptr;
};

}  // namespace ar

#endif  // AR_AUDIO_H
