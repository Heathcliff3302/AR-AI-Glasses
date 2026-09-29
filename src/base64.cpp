#include "base64.h"

namespace ar {

std::string base64Encode(const std::vector<uint8_t>& data) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz"
      "0123456789+/";

  std::string out;
  // 每 3 字节变 4 字节，先一次性把空间要够，省掉几十次 realloc。
  // 一张 640x480 的 JPEG 大概 40KB，编码后 55KB，这个 reserve 是有意义的。
  out.reserve((data.size() + 2) / 3 * 4);

  size_t i = 0;
  // 主循环一次吃满 3 个字节，凑成 24 bit 再切成 4 组 6 bit。
  for (; i + 2 < data.size(); i += 3) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += kTable[n & 0x3F];
  }

  // 收尾：剩 1 或 2 个字节时，缺的位补 0，输出端用 '=' 标明补了几个。
  const size_t rest = data.size() - i;
  if (rest == 1) {
    const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += "==";
  } else if (rest == 2) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8);
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += '=';
  }

  return out;
}

}  // namespace ar
