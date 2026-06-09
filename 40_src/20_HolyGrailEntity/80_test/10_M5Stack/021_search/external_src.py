import os
from SCons.Script import Import

Import("env")

# 【ここが重要】Arduinoフレームワークや標準ライブラリのパスを強制的にマージする
# env.get("CPPPATH", []) を再度 Append することで、WiFi.h などの場所を教えます
env.Append(CPPPATH=env.get("CPPPATH", []))

external_dirs = [
    "../../../10_common",
    "../../../20_platform/10_M5Stack"
]

for d in external_dirs:
    abs_path = os.path.abspath(d)
    dir_name = os.path.basename(abs_path)
    env.BuildSources(
        os.path.join("$BUILD_DIR", "ext_" + dir_name),
        abs_path
    )
    env.Append(CPPPATH=[abs_path])