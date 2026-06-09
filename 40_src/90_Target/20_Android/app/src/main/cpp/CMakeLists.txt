# HolyGrail Controller ネイティブ(.so)ビルド。
# Entity(10_common) + Androidプラットフォーム層(20_platform/20_Android) + astronomy.c + json をビルドする。
cmake_minimum_required(VERSION 3.22.1)
project(holygrail LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 40_src/20_HolyGrailEntity への相対パス
set(ENTITY ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../20_HolyGrailEntity)

# 共通ライブラリ部(テストmainは含まない)
file(GLOB COMMON_SRC ${ENTITY}/10_common/src/*.cpp)
# Androidプラットフォーム実装(net/ossc/tool/debugOut/jniBridge)
file(GLOB ANDROID_SRC ${ENTITY}/20_platform/20_Android/src/*.cpp)

add_library(holygrail SHARED
    ${COMMON_SRC}
    ${ANDROID_SRC}
    ${ENTITY}/10_common/lib/astronomy/astronomy.c
)

target_include_directories(holygrail PRIVATE
    ${ENTITY}/10_common/src
    ${ENTITY}/10_common/lib                 # json (json/nlohmann/json.hpp)
    ${ENTITY}/20_platform/20_Android/src
)

# 端末時刻に依存しない(時刻は自前で渡す)。fast-math は付けない(astronomy.c が #error)。
target_compile_definitions(holygrail PRIVATE ASTRONOMY_ENGINE_NO_CURRENT_TIME)
# 注: Debug で _DEBUG を定義すると DBGLN(logcat)が有効になるが、既存コードに
#     std::string/std::exception を可変長引数へ渡す箇所(clangでエラー)があるため現状は無効。
#     状態/進捗/エラー/スケジュールは通知CB経由でUIに届くので観測には支障なし。

find_library(log-lib log)
target_link_libraries(holygrail ${log-lib})
