# hgc_tjpgd (TJpgDec R0.01c 改変版)

出所: M5GFX 同梱の lgfx_tjpgd (ChaN氏の TJpgDec R0.01c を lovyan03氏がLGFX向けに改変したもの)。
エッジビルドで M5GFX 側とリンク衝突しないよう、公開関数とヘッダガードのみ改名して転載:
  lgfx_jd_prepare → hgc_jd_prepare / lgfx_jd_decomp → hgc_jd_decomp

用途: 撮影画像サムネイル(JPEG)から輝度ヒストグラムを作る測光(jpegLuma.cpp)。
ライセンス: ファイル先頭の原文どおり(再配布時は著作権表記の保持が条件)。
