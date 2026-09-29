#ifndef AR_CONFIG_H
#define AR_CONFIG_H

#include <map>
#include <string>

namespace ar {

// 全部可调参数的唯一来源。
//
// 为什么要有这一层：旧版把摄像头节点、声卡编号、模型路径、DRM 的
// connector/crtc/plane 编号、甚至 API 密钥全写死在源码里，换一台机器就得
// 改五个文件重新编译，而且密钥跟着 git 一起泄漏出去了。
//
// 现在的读取顺序是「环境变量 > 配置文件 > 内置默认值」。
// 环境变量放在最前面，是为了让 CI 和容器能在不落盘的前提下注入密钥。
// 键名到环境变量名的映射规则：api.token -> AR_API_TOKEN。
class Config {
 public:
  // 从配置文件加载。文件不存在不算错误——此时全部走默认值和环境变量，
  // 这样「只导出一个 AR_API_TOKEN 就能跑」的最小路径是通的。
  // 返回值仅表示文件是否被实际读取，供调用方打日志用。
  bool loadFile(const std::string& path);

  // 取值。找不到就返回 fallback，所以调用方永远不用判空。
  std::string getString(const std::string& key, const std::string& fallback = "") const;
  int getInt(const std::string& key, int fallback) const;
  bool getBool(const std::string& key, bool fallback) const;

  // 必填项专用：取不到就抛异常，并且异常信息里直接告诉用户该设哪个环境变量
  // 或该改配置文件的哪一行。密钥这类东西宁可启动时就明确失败，
  // 也不要带着空 token 发请求，然后拿一个 401 的 JSON 去当 AI 回答显示。
  std::string requireString(const std::string& key) const;

  // 把某个键对应的环境变量名算出来，用于错误提示。
  static std::string envNameFor(const std::string& key);

 private:
  std::map<std::string, std::string> values_;

  // 环境变量优先，其次配置文件；两边都没有就返回 nullptr。
  const std::string* lookup(const std::string& key, std::string* env_storage) const;
};

}  // namespace ar

#endif  // AR_CONFIG_H
