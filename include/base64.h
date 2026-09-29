#ifndef AR_BASE64_H
#define AR_BASE64_H

#include <cstdint>
#include <string>
#include <vector>

namespace ar {

// 标准 base64 编码(RFC 4648，带 '=' 补位)。
// 单独拆成一个文件，是因为旧版把它塞在 camera.cpp 里当静态函数，
// 而对外暴露的类却叫 base64 但干的是 JSON 打包的活，名字和职责完全错位。
std::string base64Encode(const std::vector<uint8_t>& data);

}  // namespace ar

#endif  // AR_BASE64_H
