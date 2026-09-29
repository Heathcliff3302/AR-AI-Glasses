cmake_minimum_required(VERSION 3.16)
project(ar_ai_glasses LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 不写死 Release。旧版硬编码 set(CMAKE_BUILD_TYPE Release)，
# 结果想开 -g 调试得先改 CMakeLists，而且 -DCMAKE_BUILD_TYPE=Debug 会被静默忽略。
# 这里只在用户完全没指定时给一个默认值。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# 编译时导出 compile_commands.json，clangd / clang-tidy 都要用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# 依赖
# ---------------------------------------------------------------------------

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(JPEG REQUIRED)
find_package(Freetype REQUIRED)

# 用 IMPORTED_TARGET 生成 PkgConfig::xxx 目标，而不是旧版那样
# 只取 ${DRM_LIBRARIES} 往 link 里塞。区别在于 imported target 会
# 把头文件路径、编译选项、链接选项一起带过来；只取 LIBRARIES 的话，
# 万一某个库的 pkg-config 里有必需的 -I 或 -D，就会悄悄漏掉。
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLES2 REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

# nlohmann_json 优先用系统包；没装就退回到只要头文件能找到就行，
# 这样用户自己把单头文件丢到 /usr/local/include 也能编。
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
  if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR
      "找不到 nlohmann/json.hpp。\n"
      "  Debian/Ubuntu: sudo apt install nlohmann-json3-dev\n"
      "  或手动下载单头文件到 /usr/local/include/nlohmann/json.hpp")
  endif()
endif()

# ---------------------------------------------------------------------------
# whisper.cpp
#
# 旧版把路径硬编码成 /home/firefly/hzlwkspace/whisper/whisper.cpp-master，
# 换台机器必然配置失败。现在按这个顺序找：
#   1. -DWHISPER_ROOT=<path> 显式指定
#   2. 环境变量 WHISPER_ROOT
#   3. 系统安装的 libwhisper（find_package / 标准路径）
# ---------------------------------------------------------------------------

if(NOT WHISPER_ROOT AND DEFINED ENV{WHISPER_ROOT})
  set(WHISPER_ROOT $ENV{WHISPER_ROOT})
endif()

if(WHISPER_ROOT)
  message(STATUS "使用 WHISPER_ROOT=${WHISPER_ROOT}")
  find_path(WHISPER_INCLUDE_DIR whisper.h
    HINTS "${WHISPER_ROOT}/include" "${WHISPER_ROOT}"
    NO_DEFAULT_PATH)
  # whisper.cpp 用 CMake 构建时 libwhisper 在 build/src，
  # 但不同版本放的位置不完全一样，所以多给几个候选。
  find_library(WHISPER_LIBRARY
    NAMES whisper
    HINTS "${WHISPER_ROOT}/build/src" "${WHISPER_ROOT}/build" "${WHISPER_ROOT}"
    NO_DEFAULT_PATH)
else()
  find_path(WHISPER_INCLUDE_DIR whisper.h)
  find_library(WHISPER_LIBRARY NAMES whisper)
endif()

if(NOT WHISPER_INCLUDE_DIR OR NOT WHISPER_LIBRARY)
  message(FATAL_ERROR
    "找不到 whisper.cpp。\n"
    "  先编译 whisper.cpp:\n"
    "    git clone https://github.com/ggerganov/whisper.cpp\n"
    "    cd whisper.cpp && cmake -B build -DBUILD_SHARED_LIBS=ON && cmake --build build -j\n"
    "  然后配置本项目时指定它:\n"
    "    cmake -B build -DWHISPER_ROOT=/绝对路径/whisper.cpp")
endif()
message(STATUS "whisper 头文件: ${WHISPER_INCLUDE_DIR}")
message(STATUS "whisper 库: ${WHISPER_LIBRARY}")

# ---------------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------------

add_executable(ar-ai-glasses
  src/main.cpp
  src/config.cpp
  src/base64.cpp
  src/camera.cpp
  src/audio.cpp
  src/display.cpp
  src/ai_client.cpp
)

target_include_directories(ar-ai-glasses PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${WHISPER_INCLUDE_DIR}
)
if(NLOHMANN_JSON_INCLUDE_DIR)
  target_include_directories(ar-ai-glasses PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

target_link_libraries(ar-ai-glasses PRIVATE
  ${WHISPER_LIBRARY}
  PkgConfig::DRM
  PkgConfig::GBM
  PkgConfig::EGL
  PkgConfig::GLES2
  PkgConfig::ALSA
  CURL::libcurl
  JPEG::JPEG
  Freetype::Freetype
  Threads::Threads
)
if(nlohmann_json_FOUND)
  target_link_libraries(ar-ai-glasses PRIVATE nlohmann_json::nlohmann_json)
endif()

# libwhisper.so 通常不在系统库路径里，把它的目录写进 RPATH，
# 这样不用每次都 export LD_LIBRARY_PATH。
get_filename_component(WHISPER_LIB_DIR "${WHISPER_LIBRARY}" DIRECTORY)
set_target_properties(ar-ai-glasses PROPERTIES
  BUILD_RPATH "${WHISPER_LIB_DIR}"
  INSTALL_RPATH "${WHISPER_LIB_DIR}"
)

# 警告默认打开。这类直接操作硬件和裸指针的代码，
# -Wall -Wextra 能拦住相当一部分低级错误。
target_compile_options(ar-ai-glasses PRIVATE
  -Wall -Wextra -Wpedantic
  -Wno-unused-parameter      # 信号回调之类的空参数是刻意的
)

install(TARGETS ar-ai-glasses RUNTIME DESTINATION bin)
install(FILES config.example.ini DESTINATION share/ar-ai-glasses)
