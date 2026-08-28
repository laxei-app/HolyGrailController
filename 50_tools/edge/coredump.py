# -*- coding: utf-8 -*-
"""固まった/落ちたエッジからコアダンプを取り出して読む(2026-08-28)。

【何が読めるか】panic のときに**全タスクのスタック**がフラッシュへ保存される
(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH。区画 coredump 0x7F0000 64KB は既にある)。
シリアルに1バイトも出なくても残るので、「黙って固まった」ときの唯一の手掛かりになる。

【ELF が要る】ダンプはファームと1対1で、SHA が合わないと読めない。
  Invalid application image for coredump: coredump SHA256(...) != app SHA256(...)
ビルドのたびに .pio/build/debug/elf/hgc-<版数>.elf へ控えてある(archive_elf.py)。
版数はエッジの画面右下に出ている。

  python coredump.py COM7 stick-s3            # 版数を自動で総当たり
  python coredump.py COM4 core-s3 0.1.506     # 版数を指定
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PY   = os.path.expanduser(r"~\.platformio\penv\Scripts\python.exe")
ESPTOOL = os.path.expanduser(
    r"~\.platformio\packages\tool-esptoolpy@src-d6a55a0c35a704543947cf0793b9f212\esptool.py")
GDB_DIR = os.path.expanduser(r"~\.platformio\packages\tool-xtensa-esp-elf-gdb\bin")

TARGETS = {"stick-s3": ("15_M5StickS3", 0x7F0000), "core-s3": ("10_M5Stack", 0xFF0000)}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    port, model = sys.argv[1], sys.argv[2]
    want = sys.argv[3] if len(sys.argv) > 3 else None
    if model not in TARGETS:
        raise SystemExit("機種は stick-s3 か core-s3")
    tdir, off = TARGETS[model]
    build = os.path.join(REPO, "40_src", "90_Target", tdir, ".pio", "build", "debug")

    raw = os.path.join(build, "coredump.bin")
    print("フラッシュから読み出す(0x%06X)…" % off)
    subprocess.run([PY, ESPTOOL, "--port", port, "--baud", "921600",
                    "read_flash", hex(off), "0x10000", raw], check=True)
    head = open(raw, "rb").read(4)
    if head == b"\xff\xff\xff\xff":
        print("コアダンプはありません(区画は消去状態)。panic していないということ。")
        return 0

    elfs = sorted(glob.glob(os.path.join(build, "elf", "hgc-*.elf")),
                  key=os.path.getmtime, reverse=True)
    if want:
        elfs = [e for e in elfs if want in os.path.basename(e)] or elfs
    if not elfs:
        raise SystemExit("控えた ELF がありません。そのファームのダンプは読めません")

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
    print("控えてある ELF のどれとも一致しませんでした(そのファームは残っていない)。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
