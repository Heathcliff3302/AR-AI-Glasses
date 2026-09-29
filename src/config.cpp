#include "config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace ar {
namespace {

// 去掉首尾空白。配置文件里 "key = value" 两边的空格很常见。
std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  const size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

}  // namespace

std::string Config::envNameFor(const std::string& key) {
  // api.token -> AR_API_TOKEN
  std::string out = "AR_";
  for (char c : key) {
    if (c == '.' || c == '-') {
      out += '_';
    } else {
      out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

bool Config::loadFile(const std::string& path) {
  std::ifstream in(path);
  // 用 is_open() 而不是 if(!in)：后者依赖构造失败时置 failbit，
  // 而这个行为在部分标准库实现上并不可靠(实测 MSYS2 的 libstdc++ 就不置位)。
  // is_open() 直接表达「文件到底打开了没有」，不受这种差异影响。
  if (!in.is_open()) return false;  // 没有配置文件是合法状态，不是错误

  std::string section;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;

    // [section] 会作为后续键的前缀，于是 [api] 段里的 token 就是 api.token
    if (line.front() == '[' && line.back() == ']') {
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }

    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      // 写错的行直接报出来，比默默忽略好——否则用户会盯着一个
      // 明明写了却不生效的配置项查半天。
      throw std::runtime_error(path + ":" + std::to_string(lineno) +
                               ": 缺少 '='，无法解析: " + line);
    }

    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    if (key.empty()) {
      throw std::runtime_error(path + ":" + std::to_string(lineno) + ": 键名为空");
    }
    if (!section.empty()) key = section + "." + key;
    values_[key] = val;
  }
  return true;
}

const std::string* Config::lookup(const std::string& key, std::string* env_storage) const {
  if (const char* e = std::getenv(envNameFor(key).c_str())) {
    // 空字符串的环境变量视为「没设」，避免 export AR_API_TOKEN= 之后
    // 反而把配置文件里的正确值给盖掉。
    if (*e != '\0') {
      *env_storage = e;
      return env_storage;
    }
  }
  auto it = values_.find(key);
  return it == values_.end() ? nullptr : &it->second;
}

std::string Config::getString(const std::string& key, const std::string& fallback) const {
  std::string env;
  const std::string* v = lookup(key, &env);
  return v ? *v : fallback;
}

int Config::getInt(const std::string& key, int fallback) const {
  std::string env;
  const std::string* v = lookup(key, &env);
  if (!v) return fallback;
  try {
    return std::stoi(*v);
  } catch (const std::exception&) {
    throw std::runtime_error("配置项 " + key + " 需要是整数，实际是: " + *v);
  }
}

bool Config::getBool(const std::string& key, bool fallback) const {
  std::string env;
  const std::string* v = lookup(key, &env);
  if (!v) return fallback;
  std::string s;
  for (char c : *v) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
  if (s == "0" || s == "false" || s == "no" || s == "off") return false;
  throw std::runtime_error("配置项 " + key + " 需要是布尔值(true/false)，实际是: " + *v);
}

std::string Config::requireString(const std::string& key) const {
  std::string env;
  const std::string* v = lookup(key, &env);
  if (!v || v->empty()) {
    throw std::runtime_error(
        "缺少必填配置项 \"" + key + "\"。\n" +
        "  方式一(推荐): export " + envNameFor(key) + "=<你的值>\n" +
        "  方式二: 在 config.ini 里填写该项(可从 config.example.ini 复制)");
  }
  return *v;
}

}  // namespace ar
