// HolyGrail Controller エッジ端末(M5Stack Core2) アプリ。
//
// Core2 は CoreS3 と**画面が同じ 320x240 のタッチ**で、内蔵RTC(BM8563)も microSD も持つ。
// UI の造りは完全に同じにできるので、**CoreS3 版の main.cpp を単一ソースとして再利用**する
// (プラットフォーム層と同じやり方。コピーすると 1400 行の修正を永久に二重で当てることになる)。
//
// 機種差の吸収は次の3つで行う。main.cpp を分岐で汚さない。
//  ・PMIC   : Core2=AXP192 / CoreS3=AXP2101。main.cpp 側は getType() で実行時判定しているのでそのまま動く。
//  ・電池   : batteryParams.h / batteryGuard.cpp を自機フォルダに置き、include パスの優先で差し替える。
//  ・RTC    : edgeRtc.cpp を自機フォルダに置く(Core2 は CoreS3 と同じ内蔵 BM8563 なので再利用)。
//
// Core2 だけの分岐がどうしても要るときは、platformio.ini が渡す HGC_EDGE_CORE2 を使う。
#include "../10_M5Stack/main.cpp"
