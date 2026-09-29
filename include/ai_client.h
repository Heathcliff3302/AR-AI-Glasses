#ifndef AR_AI_CLIENT_H
#define AR_AI_CLIENT_H

#include <string>
#include <vector>

#include "config.h"

namespace ar {

// 调用云端多模态模型(OpenAI 兼容的 /chat/completions 接口)。
//
// 相比旧版的三处关键修正：
//
// 1) token 不再硬编码在源码里。旧版把 sk- 开头的真实密钥直接写在 main.cpp，
//    还跟着三次提交进了 git 历史——这种泄漏删文件是没用的，必须吊销重发。
//    现在从环境变量或配置文件读。
//
// 2) 检查 HTTP 状态码。旧版只看 curl 自己的返回值(CURLE_OK)，
//    那只代表「网络层没出错」。401/429/500 的响应体会被当成正常回答，
//    一路送到 extractAIResponse，最后把一段错误 JSON 画到屏幕上当 AI 回答。
//
// 3) 加超时和重试。旧版没有任何超时设置，网络卡住就无限期挂着；
//    429(限流)和 5xx(服务端临时故障)都是重试就能好的，值得自动退避重试。
class AiClient {
 public:
  explicit AiClient(const Config& cfg);
  ~AiClient();

  AiClient(const AiClient&) = delete;
  AiClient& operator=(const AiClient&) = delete;

  // 把若干张 base64 图片和一段文字提示一起发给模型，返回模型的文字回答。
  // 失败(网络不通、认证失败、响应格式不认识)一律抛异常，
  // 绝不会把错误信息伪装成"回答"返回。
  std::string ask(const std::vector<std::string>& base64_jpegs, const std::string& prompt);

 private:
  // 组装 OpenAI 多模态格式的请求体
  std::string buildRequestBody(const std::vector<std::string>& base64_jpegs,
                               const std::string& prompt) const;

  // 发一次 HTTP POST。返回 body，HTTP 状态码写到 status_out。
  // 网络层错误抛异常。
  std::string postOnce(const std::string& body, long* status_out) const;

  // 从响应里取出 choices[0].message.content
  static std::string extractContent(const std::string& response_json);

  std::string url_;
  std::string token_;
  std::string model_;
  int timeout_sec_ = 0;
  int connect_timeout_sec_ = 0;
  int max_retries_ = 0;
  std::string image_detail_;
};

}  // namespace ar

#endif  // AR_AI_CLIENT_H
