# エッジ端末のバージョンを 40_src/version.properties から読み、ビルドのたびに
# パッチを +1 して edgeVersion.h を生成する(2026-08-08 UI依頼)。
#
# major/minor はスマホと共有(手で更新)。パッチはエッジ側だけを進める。
# CoreS3 と StickS3 は同じ「エッジ端末」なので、両者で1つのパッチ系列を共有する
# (どちらをビルドしても +1 され、書き込んだ機体は同じ番号になる)。
Import("env")
import os, re

# extra_scripts は exec() で読み込まれるため __file__ が無い。プロジェクト位置から辿る。
#   PROJECT_DIR = 40_src/90_Target/<機種>  →  40_src は2つ上。
PROJ  = env.subst("$PROJECT_DIR")
SRC   = os.path.abspath(os.path.join(PROJ, "..", ".."))        # 40_src
PROPS = os.path.join(SRC, "version.properties")
OUT   = os.path.join(SRC, "10_UI", "18_M5Common", "edgeVersion.h")

def read_props(path):
    d = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            d[k.strip()] = v.strip()
    return d

def write_patch(path, key, value):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"(?m)^%s\s*=.*$" % re.escape(key), "%s=%d" % (key, value), text)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

p     = read_props(PROPS)
major = int(p.get("major", 0))
minor = int(p.get("minor", 0))
# 同じ版数を複数台へ焼くための抑止スイッチ。upload はそのたびにビルドが走るので、
# 素直に +1 すると CoreS3 が2台あるだけで別々の版数になってしまう(2026-08-11)。
# 2台目以降は HGC_NO_BUMP=1 を付けて焼く。
if os.environ.get("HGC_NO_BUMP") == "1":
    patch = int(p.get("edgePatch", 0))
else:
    patch = int(p.get("edgePatch", 0)) + 1
    write_patch(PROPS, "edgePatch", patch)

ver = "%d.%d.%d" % (major, minor, patch)
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("// 自動生成(bump_version.py)。手で編集しない。\n")
    f.write("// 版数は 40_src/version.properties で管理し、ビルドのたびにパッチが +1 される。\n")
    f.write("#ifndef _EDGE_VERSION_H_\n#define _EDGE_VERSION_H_\n")
    f.write('#define HGC_EDGE_VERSION "%s"\n' % ver)
    f.write("#endif\n")
print("[version] edge %s -> %s" % (ver, OUT))
