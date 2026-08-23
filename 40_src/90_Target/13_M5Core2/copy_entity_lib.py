# ビルド後フック: HolyGrailEntity(10_common)の静的アーカイブを
# 分かりやすい名前 libHolyGrailEntity.a として $BUILD_DIR 直下にコピーする。
# PlatformIO はソースライブラリのアーカイブをフォルダ名由来で libsrc.a と命名するため、
# HolyGrailEntity の核(holyGrailEntity.cpp.o を含む libsrc.a)を特定してコピーする。
import os
import shutil

Import("env")

def emit_entity_archive(*args, **kwargs):
    build_dir = env.subst("$BUILD_DIR")
    for name in os.listdir(build_dir):
        d = os.path.join(build_dir, name)
        archive = os.path.join(d, "libsrc.a")
        marker = os.path.join(d, "src", "holyGrailEntity.cpp.o")  # 核の目印
        if os.path.isfile(archive) and os.path.isfile(marker):
            dst = os.path.join(build_dir, "libHolyGrailEntity.a")
            shutil.copyfile(archive, dst)
            print("HolyGrailEntity static lib -> %s" % dst)
            return
    print("WARN: HolyGrailEntity archive (libsrc.a with holyGrailEntity.cpp.o) not found")

# 最終リンク(.elf)後に実行する
env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", emit_entity_archive)
