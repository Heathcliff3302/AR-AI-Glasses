// AR-AI-Glasses: 可穿戴 AR 设备的多模态实时交互原型
//
// 一次交互的完整流程：
//   按 s
//     ├─ 线程 A: 摄像头采集 10 秒，每 500ms 取一帧 -> JPEG -> base64
//     └─ 线程 B: 麦克风录音 10 秒 -> whisper 本地转文字
//   两路都完成后
//     -> 打包成 OpenAI 多模态格式的 JSON
//     -> HTTP POST 给云端视觉模型
//     -> 取出回答文字
//     -> 通过 DRM/KMS 直接画到屏幕上(不经过任何窗口系统)

#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ai_client.h"
#include "audio.h"
#include "camera.h"
#include "config.h"
#include "display.h"

namespace {

// 只用于让主循环知道该退出。signal handler 里能安全碰的东西极少，
// volatile sig_atomic_t 是标准允许的那一种。
volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

// 一次采集的结果。两个线程各写一半，主线程等齐了再读。
struct CaptureResult {
  std::vector<std::string> images;
  std::string transcript;
  // 线程里抛出的异常没法直接跨线程传播，先存下来，
  // 回到主线程再 rethrow。旧版没有这个机制，采集线程里一旦出异常
  // 就是 std::terminate，整个程序直接崩。
  std::exception_ptr image_error;
  std::exception_ptr audio_error;
};

// 跑一次完整的「采集 -> 推理 -> 显示」。
// 任何一步失败都抛异常，由调用方决定是退出还是继续等下一次输入。
void runOnce(ar::Camera& camera, ar::SpeechToText& stt, ar::AiClient& ai,
             ar::DrmDisplay& display) {
  CaptureResult result;

  std::mutex mtx;
  std::condition_variable cv;
  bool image_done = false;
  bool audio_done = false;

  // 图像和音频是两个完全独立的硬件，串行做要 20 秒，并行只要 10 秒。
  // 旧版这部分的并发写得是对的(通知在持锁内发、wait 带谓词防虚假唤醒)，
  // 这里保持同样的结构，只补上异常捕获。
  std::thread t_image([&] {
    try {
      result.images = camera.captureBurst();
    } catch (...) {
      result.image_error = std::current_exception();
    }
    {
      std::lock_guard<std::mutex> lk(mtx);
      image_done = true;
    }
    cv.notify_one();
  });

  std::thread t_audio([&] {
    try {
      result.transcript = stt.recordAndTranscribe();
    } catch (...) {
      result.audio_error = std::current_exception();
    }
    {
      std::lock_guard<std::mutex> lk(mtx);
      audio_done = true;
    }
    cv.notify_one();
  });

  {
    std::unique_lock<std::mutex> lk(mtx);
    cv.wait(lk, [&] { return image_done && audio_done; });
  }
  t_image.join();
  t_audio.join();

  // 两个线程的异常都在这里统一抛出。先看图像：没有图像这次交互就没意义了。
  if (result.image_error) std::rethrow_exception(result.image_error);
  if (result.audio_error) std::rethrow_exception(result.audio_error);

  if (result.images.empty()) {
    throw std::runtime_error("一帧图像都没采到，检查摄像头是否被其他程序占用");
  }

  std::cout << "[采集] 图像 " << result.images.size() << " 帧，识别文本: \""
            << result.transcript << "\"" << std::endl;

  // 语音没识别出内容时给一个默认提问，而不是发一个空 prompt 过去
  // ——空 prompt 下模型的输出基本不可控。
  std::string prompt = result.transcript;
  if (prompt.empty()) {
    prompt = "请简要描述这些画面里的内容，并指出值得注意的地方。";
    std::cout << "[采集] 未识别到语音，使用默认提问" << std::endl;
  }

  const std::string reply = ai.ask(result.images, prompt);
  std::cout << "[AI] 回答: " << reply << std::endl;

  display.showText(reply);
  std::cout << "[显示] 已上屏" << std::endl;
}

}  // namespace

int main() {
  // Ctrl+C 走正常退出路径，让所有析构函数跑完——
  // 特别是 DrmDisplay 要把 CRTC 还原回去，否则退出后屏幕是一片黑，
  // 用户会以为机器死了。
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  try {
    ar::Config cfg;
    // 配置文件路径可以用 AR_CONFIG_FILE 覆盖，默认找当前目录的 config.ini。
    // 文件不存在不算错误——全用环境变量跑是被支持的用法。
    const char* cfg_env = std::getenv("AR_CONFIG_FILE");
    const std::string cfg_path = cfg_env ? cfg_env : "config.ini";
    if (cfg.loadFile(cfg_path)) {
      std::cout << "[配置] 已加载 " << cfg_path << std::endl;
    } else {
      std::cout << "[配置] 未找到 " << cfg_path << "，使用默认值和环境变量" << std::endl;
    }

    // 构造顺序就是依赖顺序，任何一个失败都会带着清晰的错误信息退出，
    // 而且已经构造好的对象会按逆序正确析构。
    // 这几个初始化都比较慢(whisper 要加载 140MB 模型、DRM 要枚举资源)，
    // 所以全部前置到启动阶段做一次，而不是每次交互都重来。
    ar::AiClient ai(cfg);
    ar::SpeechToText stt(cfg);
    ar::Camera camera(cfg);
    ar::DrmDisplay display(cfg);

    std::cout << "\n就绪。按 s + 回车 开始一次采集，按 q + 回车 退出。" << std::endl;
    display.showText("就绪\n按 s 开始采集");

    while (!g_stop) {
      const int c = std::getchar();
      if (c == EOF) break;  // 管道输入结束或收到信号
      if (c == '\n' || c == '\r') continue;

      if (c == 'q' || c == 'Q') break;

      if (c == 's' || c == 'S') {
        try {
          runOnce(camera, stt, ai, display);
        } catch (const std::exception& e) {
          // 单次交互失败不该让程序退出：摄像头被占用、网络抖动、
          // 限流，这些下一次就可能好了。把错误显示到屏幕上，等用户再按 s。
          std::cerr << "[错误] " << e.what() << std::endl;
          try {
            display.showText(std::string("出错了:\n") + e.what());
          } catch (const std::exception& de) {
            // 连显示都失败了，那就只能打到终端
            std::cerr << "[错误] 显示错误信息也失败了: " << de.what() << std::endl;
          }
        }
      }
    }

    std::cout << "退出。" << std::endl;
    return 0;

  } catch (const std::exception& e) {
    // 初始化阶段的失败走到这里。错误信息里已经带了排查提示。
    std::cerr << "\n[启动失败] " << e.what() << std::endl;
    return 1;
  }
}
