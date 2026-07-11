# Arduino-ESP32 3.x(pioarduino)では組込みライブラリ同士が互いのヘッダを include する
# (WiFi→Network、LittleFS/SD→FS、… )。本プロジェクトの構成では PlatformIO の LDF が
# これらの相互依存を解決しきれず "Network.h/FS.h not found" になる。
# 対策として framework の libraries/*/src をすべて CPPPATH に追加し、相互 include を通す。
Import("env")
import glob
import os

fw = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
if fw:
    added = 0
    for d in sorted(glob.glob(os.path.join(fw, "libraries", "*", "src"))):
        env.Append(CPPPATH=[d])
        added += 1
    print("[add_fw_includes] added %d framework library src dirs to CPPPATH" % added)
