# -*- coding: utf-8 -*-
"""ビルドのたびに firmware.elf を版数で控える(2026-08-28)。

【なぜ要るか】コアダンプ(panic 時に全タスクのスタックがフラッシュへ残る仕組み)は、
そのとき動いていたファームの ELF が無いと**一切読めない**。実際に読み出そうとして

    Invalid application image for coredump: coredump SHA256(...) != app SHA256(...)

で止まった。PlatformIO は firmware.elf を毎回上書きするので、何度か焼き直した後では
もう戻せない。版数付きで控えておけば、後からダンプを解ける。

置き場所は .pio/build/<env>/elf/hgc-<版数>.elf。ビルド成果物の中なので git には入らない。
古い物は kKeep 個まで残して消す(1つ50MB弱あるため)。
"""
Import("env")
import os
import re
import shutil

kKeep = 12


def elfArchive(source, target, env):
    build = env.subst("$BUILD_DIR")
    src = os.path.join(build, "firmware.elf")
    if not os.path.exists(src):
        return
    verPath = os.path.join(env.subst("$PROJECT_DIR"), "..", "..",
                           "10_UI", "18_M5Common", "edgeVersion.h")
    ver = "unknown"
    try:
        with open(verPath, encoding="utf-8-sig") as f:
            m = re.search(r'HGC_EDGE_VERSION\s+"([^"]+)"', f.read())
            if m:
                ver = m.group(1)
    except Exception:
        pass
    outDir = os.path.join(build, "elf")
    os.makedirs(outDir, exist_ok=True)
    dst = os.path.join(outDir, "hgc-%s.elf" % ver)
    try:
        shutil.copyfile(src, dst)
    except Exception:
        return
    # 古い物を落とす(更新時刻の新しい順に kKeep 個だけ残す)
    try:
        files = [os.path.join(outDir, n) for n in os.listdir(outDir) if n.endswith(".elf")]
        files.sort(key=os.path.getmtime, reverse=True)
        for p in files[kKeep:]:
            os.remove(p)
    except Exception:
        pass


env.AddPostAction("$BUILD_DIR/firmware.bin", elfArchive)
