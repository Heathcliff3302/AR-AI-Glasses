#include "ai_client.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace ar {
namespace {

using json = nlohmann::json;

size_t writeCallback(void* data, size_t size, size_t nmemb, void* userp) {
  const size_t total = size * nmemb;
  static_cast<std::string*>(userp)->append(static_cast<char*>(data), total);
  return total;
}

// 把密钥从日志里遮掉。调试时需要看到「token 确实读到了」，
// 但完整打出来就等于换了个地方泄漏一次。
std::string maskToken(const std::string& t) {
  if (t.size() <= 8) return "****";
  return t.substr(0, 4) + "..." + t.substr(t.size() - 4);
}

// 这些状态码重试有意义：限流和服务端临时故障。
// 4xx 里除了 429 都是请求本身有问题(密钥错、参数错)，重试只是浪费时间。
bool isRetryable(long status) {
  return status == 429 || status == 408 || (status >= 500 && status < 600);
}

}  // namespace

AiClient::AiClient(const Config& cfg) {
  url_ = cfg.getString("api.url", "https://api.siliconflow.cn/v1/chat/completions");
  model_ = cfg.getString("api.model", "Qwen/Qwen3-VL-32B-Thinking");
  timeout_sec_ = cfg.getInt("api.timeout_sec", 120);
  connect_timeout_sec_ = cfg.getInt("api.connect_timeout_sec", 10);
  max_retries_ = cfg.getInt("api.max_retries", 2);
  image_detail_ = cfg.getString("api.image_detail", "low");

  // 必填项，取不到直接抛，错误信息里会告诉用户设哪个环境变量
  token_ = cfg.requireString("api.token");

  // curl 的全局初始化不是线程安全的，必须在起线程之前做一次。
  // 放在这里是因为 AiClient 在 main 里、在采集线程启动之前就构造好了。
  const CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (rc != CURLE_OK) {
    throw std::runtime_error(std::string("curl_global_init 失败: ") + curl_easy_strerror(rc));
  }

  std::cout << "[AI] 模型=" << model_ << " token=" << maskToken(token_) << std::endl;
}

AiClient::~AiClient() { curl_global_cleanup(); }

std::string AiClient::buildRequestBody(const std::vector<std::string>& base64_jpegs,
                                       const std::string& prompt) const {
  json content = json::array();

  for (const std::string& b64 : base64_jpegs) {
    content.push_back({{"type", "image_url"},
                       {"image_url",
                        {{"url", "data:image/jpeg;base64," + b64},
                         // detail 直接决定单图消耗多少 token。
                         // 旧版写死 "high"，20 张图叠起来很容易撞上
                         // 单次请求的 token 上限，表现为莫名其妙的 400。
                         // 默认给 low，需要细节再从配置里调。
                         {"detail", image_detail_}}}});
  }

  // 文字放最后：多模态模型通常把靠后的指令看得更重，
  // 而且先给素材再提问更符合「看这些图，回答这个问题」的语义。
  content.push_back({{"type", "text"}, {"text", prompt}});

  json payload;
  payload["model"] = model_;
  payload["messages"] = json::array({{{"role", "user"}, {"content", content}}});
  return payload.dump();
}

std::string AiClient::postOnce(const std::string& body, long* status_out) const {
  CURL* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("curl_easy_init 失败");

  // 用一个小 guard 保证不管从哪条路退出，curl 句柄和 header 链表都会释放
  struct Guard {
    CURL* c;
    curl_slist* h = nullptr;
    ~Guard() {
      if (h) curl_slist_free_all(h);
      if (c) curl_easy_cleanup(c);
    }
  } guard{curl};

  const std::string auth = "Authorization: Bearer " + token_;
  guard.h = curl_slist_append(guard.h, auth.c_str());
  guard.h = curl_slist_append(guard.h, "Content-Type: application/json");
  if (!guard.h) throw std::runtime_error("构造 HTTP 头失败");

  std::string response;
  char errbuf[CURL_ERROR_SIZE] = {0};

  curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, guard.h);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  // 用 _LARGE 版本并显式给长度：请求体里有几十张 base64 图片，
  // 轻松超过 2GB 以下但远超默认假设的小 body；给了长度 curl 就不用自己 strlen，
  // 也避免了 body 里万一有 \0 被截断。
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

  // 证书校验必须开。这是旧版做对的地方，保留。
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  // 超时是旧版完全没有的。没有超时意味着网络一卡，整个程序就永久挂在这里，
  // 用户只能 Ctrl+C。
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec_));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(connect_timeout_sec_));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // 多线程环境下必须设，否则超时用信号实现会乱

  const CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    throw std::runtime_error(std::string("HTTP 请求失败: ") +
                             (errbuf[0] ? errbuf : curl_easy_strerror(rc)));
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_out);
  return response;
}

std::string AiClient::extractContent(const std::string& response_json) {
  json j;
  try {
    j = json::parse(response_json);
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("AI 响应不是合法 JSON: ") + e.what());
  }

  // 服务端把错误也放在 200 里返回是常见做法，所以先看有没有 error 字段
  if (j.contains("error")) {
    const auto& err = j["error"];
    const std::string msg = err.is_object() && err.contains("message")
                                ? err["message"].get<std::string>()
                                : err.dump();
    throw std::runtime_error("AI 接口返回错误: " + msg);
  }

  // 用 at() 之前逐层检查类型。响应是外部数据，不能假设结构一定对——
  // 直接 j["choices"][0]["message"]["content"] 在结构不符时行为很难预期。
  if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
    throw std::runtime_error("AI 响应里没有 choices 字段: " + response_json.substr(0, 300));
  }
  const auto& first = j["choices"][0];
  if (!first.contains("message") || !first["message"].contains("content")) {
    throw std::runtime_error("AI 响应缺少 message.content: " + response_json.substr(0, 300));
  }
  const auto& content = first["message"]["content"];
  if (!content.is_string()) {
    throw std::runtime_error("AI 响应的 content 不是字符串");
  }
  return content.get<std::string>();
}

std::string AiClient::ask(const std::vector<std::string>& base64_jpegs,
                          const std::string& prompt) {
  if (base64_jpegs.empty() && prompt.empty()) {
    throw std::runtime_error("图像和文字都是空的，没有可发送的内容");
  }

  const std::string body = buildRequestBody(base64_jpegs, prompt);
  std::cout << "[AI] 请求体 " << (body.size() / 1024) << " KB，图片 "
            << base64_jpegs.size() << " 张" << std::endl;

  std::string last_error;
  for (int attempt = 0; attempt <= max_retries_; ++attempt) {
    if (attempt > 0) {
      // 指数退避：1s、2s、4s...。固定间隔重试在服务端限流时
      // 只会加重拥塞，翻倍等待是标准做法。
      const int wait_sec = 1 << (attempt - 1);
      std::cout << "[AI] 第 " << attempt << " 次重试，等待 " << wait_sec << " 秒..."
                << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(wait_sec));
    }

    long status = 0;
    std::string response;
    try {
      response = postOnce(body, &status);
    } catch (const std::exception& e) {
      // 网络层错误(超时、DNS 失败)也值得重试
      last_error = e.what();
      continue;
    }

    if (status == 200) {
      return extractContent(response);
    }

    // 到这里说明是 HTTP 错误状态。旧版根本不看状态码，
    // 会把下面这段错误 JSON 当成 AI 的回答显示到屏幕上。
    last_error = "HTTP " + std::to_string(status) + ": " + response.substr(0, 300);

    if (status == 401 || status == 403) {
      throw std::runtime_error(
          "认证失败(HTTP " + std::to_string(status) + ")。\n"
          "  提示: 检查 AR_API_TOKEN 是否正确、是否已过期或被吊销");
    }
    if (!isRetryable(status)) {
      throw std::runtime_error("AI 接口返回不可重试的错误: " + last_error);
    }
  }

  throw std::runtime_error("AI 接口重试 " + std::to_string(max_retries_) +
                           " 次后仍然失败: " + last_error);
}

}  // namespace ar
