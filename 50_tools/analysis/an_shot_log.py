# -*- coding: utf-8 -*-
# スマホのログ(hg_<日付>.log)から SHOT 行を取り出して 1コマ1行の TSV にする。
# an_shot_linear.py が出す実写の測定値と、an_shot_report.py で突き合わせる。
#
# 取り出すもの: 撮影時刻 / 通し番号 / ISO / ss / F / 適用ev / 撮影制御方法(窓) /
#               そのコマの測光リニア輝度Y / 18%基準の露出誤差
#
# 【ev の読み方(注意)】ログの ev は **18%(0.18)を基準にした値**。ところが薄明の
#  目標輝度は ev0 シグモイドで 0.18→0.06 まで下がる(exposureMath.h の linearLo)。
#  そのため preNight/postNight では ev が -2段 などと出るが、それは狙いからの
#  ずれではない。狙いとの差は an_shot_report.py が ev0 を計算して出す。
#
# 使い方:
#   python an_shot_log.py <ログファイル> <日付 YYYY-MM-DD> <morning|evening> <出力TSV>
# 例:
#   python an_shot_log.py _retrieved_logs/hg_2026-09-02.log.txt 2026-09-02 morning log_0902.tsv
#
# morning/evening は「1つのログに朝と夕の2本が入っている」ときの選り分け。
#  morning=12時より前、evening=12時以降。
import io, re, sys

if len(sys.argv) < 5:
    print("usage: an_shot_log.py <ログ> <YYYY-MM-DD> <morning|evening> <出力TSV>")
    raise SystemExit(1)
LOG, DATE, WHEN, OUT = sys.argv[1], sys.argv[2], sys.argv[3].lower(), sys.argv[4]
if WHEN not in ("morning", "evening"):
    print("3番目は morning か evening")
    raise SystemExit(1)

# 例: 2026-09-02 04:32:14|INF|SHOT  |  255|  250|8          |1.4   |  +6.000|postNight Y=0.0704 ev+0.23 ... sh=04:32:14.123
RE = re.compile(r"^(" + re.escape(DATE) +
                r") (\d\d:\d\d:\d\d)\|INF\|SHOT  \|\s*(\d+)\|\s*(\d+)\|([^|]+)\|([^|]+)\|\s*([-+0-9.]+)\|(\S+)(.*)$")


def ss_sec(s):
    """"1/25" や "8" を秒へ。"""
    s = s.strip()
    if s.startswith("1/"):
        return 1.0 / float(s[2:])
    return float(s.rstrip('"'))


rows = []
for line in io.open(LOG, encoding="utf-8", errors="replace"):
    m = RE.match(line)
    if not m:
        continue
    hh = int(m.group(2)[:2])
    if (WHEN == "morning") != (hh < 12):
        continue
    tail = m.group(9)
    y = re.search(r"Y=([0-9.]+)", tail)
    ev = re.search(r"ev([-+][0-9.]+)", tail)
    sh = re.search(r"sh=(\d\d:\d\d:\d\d)", tail)
    rows.append((m.group(2), sh.group(1) if sh else m.group(2), int(m.group(3)), int(m.group(4)),
                 ss_sec(m.group(5)), float(m.group(6)), float(m.group(7)), m.group(8),
                 y.group(1) if y else "", ev.group(1) if ev else ""))

if not rows:
    print("SHOT 行が見つかりません。日付と morning/evening を確かめてください。")
    raise SystemExit(1)

w = io.open(OUT, "w", encoding="utf-8", newline="\n")
w.write("logtime\tshtime\tfr\tiso\tss\tfn\tevset\tccm\tYmeter\tevadj\n")
for r in rows:
    w.write("\t".join(str(x) for x in r) + "\n")
w.close()
print("rows", len(rows), rows[0][0], "->", rows[-1][0], "->", OUT)
