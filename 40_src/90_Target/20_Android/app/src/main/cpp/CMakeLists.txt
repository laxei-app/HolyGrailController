# HolyGrail Controller ネイティブ(.so)ビルド。
# Entity(10_common) + Androidプラットフォーム層(20_platform/20_Android) + astronomy.c + json をビルドする。
cmake_minimum_required(VERSION 3.22.1)
project(HolyGrailEntity LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 40_src/20_HolyGrailEntity への相対パス
set(ENTITY ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../20_HolyGrailEntity)

# 共通ライブラリ部(テストmainは含まない)。CONFIGURE_DEPENDS でファイル追加時に再評価。
file(GLOB COMMON_SRC CONFIGURE_DEPENDS ${ENTITY}/10_common/src/*.cpp)
# Androidプラットフォーム実装(net/ossc/tool/debugOut/jniBridge)
file(GLOB ANDROID_SRC CONFIGURE_DEPENDS ${ENTITY}/20_platform/20_Android/src/*.cpp)
# 役割別コード: スマホ役の実装だけをリンクする(§4.4。スマホ成果物 = common + 30_role/10_phone + platform/Android)。
file(GLOB PHONE_ROLE_SRC CONFIGURE_DEPENDS ${ENTITY}/30_role/10_phone/src/*.cpp)

add_library(HolyGrailEntity SHARED
    ${COMMON_SRC}
    ${ANDROID_SRC}
    ${PHONE_ROLE_SRC}
    ${ENTITY}/10_common/lib/astronomy/astronomy.c
    ${ENTITY}/10_common/lib/jpeg/hgc_tjpgd.c
)

target_include_directories(HolyGrailEntity PRIVATE
    ${ENTITY}/10_common/src
    ${ENTITY}/10_common/lib                 # json (json/nlohmann/json.hpp)
    ${ENTITY}/20_platform/20_Android/src
    ${ENTITY}/30_role                       # 役割別の窓口 roleDiscovery.h
)

# 端末時刻に依存しない(時刻は自前で渡す)。fast-math は付けない(astronomy.c が #error)。
target_compile_definitions(HolyGrailEntity PRIVATE ASTRONOMY_ENGINE_NO_CURRENT_TIME)
# 注: Debug で _DEBUG を定義すると DBGLN(logcat)が有効になるが、既存コードに
#     std::string/std::exception を可変長引数へ渡す箇所(clangでエラー)があるため現状は無効。
#     状態/進捗/エラー/スケジュールは通知CB経由でUIに届くので観測には支障なし。

find_library(log-lib log)
target_link_libraries(HolyGrailEntity ${log-lib})
