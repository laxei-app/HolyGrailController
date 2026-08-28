# -*- coding: utf-8 -*-
"""固まった/落ちたエッジからコアダンプを取り出して読む(2026-08-28)。

【何が読めるか】panic のときに**全タスクのスタック**がフラッシュへ保存される
(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH は framework 側で既に有効。区画も既定の表にある)。
シリアルに1バイトも出なくても残るので、「黙って固まった」ときの唯一の手掛かりになる。

【機種を問わない】区画の番地は**端末の区画表(0x8000)を読んで探す**。CoreS3(16MB)は
0xFF0000、StickS3(8MB)は 0x7F0000 だが、ここを手で持つと機種や設定を変えたときに黙って
外れる。表から引けば当たり続ける。ELF も両機種のビルド先から新しい順に総当たりする。

【ELF が要る】ダンプはファームと1対1で、SHA が合わないと読めない。
  Invalid application image for coredump: coredump SHA256(...) != app SHA256(...)
ビルドのたびに .pio/build/debug/elf/hgc-<版数>.elf へ控えてある(archive_elf.py)。
版数はエッジの画面右下に出ている。

  python coredump.py COM7             # 機種は自動(区画表とELFの両方を総当たり)
  python coredump.py COM4 0.1.506     # 版数が分かっているなら指定すると速い
  python coredump.py COM7 --erase     # 古いダンプを消す

【古いダンプは消しておくこと】新しい panic は古いダンプを上書きするが、panic が
起きなければ古い物が残り続ける。ELF がもう無いダンプが残っていると「読めないダンプ」を
毎回掴んで紛らわしい。仕掛けを入れた時点で一度消しておけば、以後見つかったダンプは
必ず新しく、必ず読める。
"""
import glob
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PY   = os.path.expanduser(r"~\.platformio\penv\Scripts\python.exe")
ESPTOOL = os.path.expanduser(
    r"~\.platformio\packages\tool-esptoolpy@src-d6a55a0c35a704543947cf0793b9f212\esptool.py")
GDB_DIR = os.path.expanduser(r"~\.platformio\packages\tool-xtensa-esp-elf-gdb\bin")

BUILD_DIRS = ["15_M5StickS3", "10_M5Stack"]

PART_TABLE_OFF  = 0x8000
PART_TABLE_SIZE = 0xC00
PART_MAGIC      = 0x50AA
TYPE_DATA       = 0x01
SUBTYPE_COREDUMP = 0x03


def readFlash(port, off, size, out):
    subprocess.run([PY, ESPTOOL, "--port", port, "--baud", "921600",
                    "read_flash", hex(off), hex(size), out], check=True)


def findCoredumpPartition(blob):
    """区画表から coredump の (offset, size) を返す。見つからなければ None。"""
    for i in range(0, len(blob), 32):
        e = blob[i:i + 32]
        if len(e) < 32:
            break
        magic, ptype, subtype, off, size = struct.unpack("<HBBII", e[:12])
        if magic != PART_MAGIC:
            continue                      # 表の終わり(MD5行や 0xFF 埋め)
        if ptype == TYPE_DATA and subtype == SUBTYPE_COREDUMP:
            name = e[12:28].split(b"\x00")[0].decode("ascii", "replace")
            return off, size, name
    return None


def elfCandidates(want):
    out = []
    for t in BUILD_DIRS:
        d = os.path.join(REPO, "40_src", "90_Target", t, ".pio", "build", "debug", "elf")
        out += glob.glob(os.path.join(d, "hgc-*.elf"))
    out.sort(key=os.path.getmtime, reverse=True)
    if want:
        hit = [e for e in out if want in os.path.basename(e)]
        if hit:
            return hit
        print("版数 %s の控えが見つかりません。ある物を総当たりします。" % want)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    port = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else None
    tmp  = os.path.join(HERE, "_coredump_tmp")
    os.makedirs(tmp, exist_ok=True)

    ptab = os.path.join(tmp, "parttable.bin")
    print("区画表を読む(0x%06X)…" % PART_TABLE_OFF)
    readFlash(port, PART_TABLE_OFF, PART_TABLE_SIZE, ptab)
    found = findCoredumpPartition(open(ptab, "rb").read())
    if found is None:
        raise SystemExit("この端末には coredump 区画がありません。区画表を確認してください。")
    off, size, name = found
    print("見つけた区画: %s off=0x%06X size=0x%X" % (name, off, size))

    if want == "--erase":
        print("ダンプを消す(0x%06X, 0x%X)…" % (off, size))
        subprocess.run([PY, ESPTOOL, "--port", port, "--baud", "921600",
                        "erase_region", hex(off), hex(size)], check=True)
        print("消しました。以後に見つかるダンプは新しい物だけです。")
        return 0

    raw = os.path.join(tmp, "coredump.bin")
    readFlash(port, off, size, raw)
    head = open(raw, "rb").read(4)
    if head == b"\xff\xff\xff\xff":
        print("コアダンプはありません(区画は消去状態)。一度も panic していないということ。")
        return 0
    n = struct.unpack("<I", head)[0]
    print("ダンプあり: %d バイト" % n)

    elfs = elfCandidates(want)
    if not elfs:
        raise SystemExit("控えた ELF が1つもありません。そのファームのダンプは読めません。")

    env = dict(os.environ)
    env["PATH"] = GDB_DIR + os.pathsep + env.get("PATH", "")
    for e in elfs:
        print("ELF を試す: %s" % os.path.basename(e))
        r = subprocess.run([sys.executable, "-m", "esp_coredump", "--chip", "esp32s3",
                            "info_corefile", "--core", raw, "--core-format", "raw", e],
                           env=env, capture_output=True)
        out = r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")
        if "Invalid application image" in out:
            continue
        print(out)
        return 0
    print("控えてある ELF のどれとも一致しませんでした(そのファームがもう残っていない)。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
