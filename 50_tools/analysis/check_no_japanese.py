# -*- coding: utf-8 -*-
# Entity と通信路に日本語が混ざっていないかを数える。**コメントは対象外**、文字列リテラルだけ見る。
#
# 方針(2026-08-22): 将来の多言語対応を UI 側の差し替えだけで済ませるため、Entity の中と
#  通信路には日本語を置かない。ログはどの言語設定でも英語。UIに出す文言は hgc::notice の
#  コードで流し、Android の noticeText() だけが文言を持つ。
#  → 詳細は commit f6a4e8d のメッセージ。
#
# 使い方: python check_no_japanese.py   … 0件なら方針を満たしている。
#
# 注意: 判定は「文字列リテラルの中に日本語があるか」だけ。snprintf で組み立ててから
#  logEvent/onNotice_ へ渡す形だと、この分類だけでは行き先が分からない。**行き先は
#  snprintf ではなく、その後の渡し先で決まる**(2026-08-22 にこれで分類を1度誤った)。
import os, re, sys, io

ROOT = r'Z:/projects/cameraControl/projects/HolyGrailController/HolyGrailController/40_src'

JP = re.compile(u'[\u3040-\u309f\u30a0-\u30ff\u4e00-\u9fff\uff01-\uff60\u3000-\u303f]')


def strip_comments_keep_strings(src):
    """コメントを空白へ潰し、文字列リテラルはそのまま残す。行番号を保つため改行は維持。"""
    out = []
    i, n = 0, len(src)
    line_starts = [0]
    while i < n:
        c = src[i]
        if c == '"':                      # 文字列
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == '"':
                    break
                if src[j] == '\n':
                    break
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
            continue
        if c == "'":                      # 文字リテラル
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1])
            i = j + 1
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in src[i:j]))
            i = j
            continue
        out.append(c)
        i += 1
    return ''.join(out)


STRLIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def scan(path):
    try:
        src = io.open(path, encoding='utf-8-sig').read()
    except Exception:
        return []
    code = strip_comments_keep_strings(src)
    hits = []
    for m in STRLIT.finditer(code):
        lit = m.group(0)
        if not JP.search(lit):
            continue
        line = code.count('\n', 0, m.start()) + 1
        hits.append((line, lit))
    return hits


GROUPS = [
    ('Entity 共通(全機種で動く中身)',      '20_HolyGrailEntity/10_common/src'),
    ('Entity 役割別(スマホ専用など)',       '20_HolyGrailEntity/30_role'),
    ('Entity プラットフォーム層',           '20_HolyGrailEntity/20_platform'),
    ('エッジのUI(M5Stack CoreS3)',        '10_UI/10_M5Stack'),
    ('エッジのUI(M5StickS3)',             '10_UI/15_M5StickS3'),
    ('エッジのUI(共通)',                   '10_UI/18_M5Common'),
]

total = 0
for title, rel in GROUPS:
    base = os.path.join(ROOT, rel).replace('\\', '/')
    rows = []
    for dp, _dn, fns in os.walk(base):
        if '80_test' in dp or '\\build' in dp or '/build' in dp or '.pio' in dp:
            continue
        for fn in fns:
            if not fn.endswith(('.cpp', '.h', '.c', '.hpp')):
                continue
            p = os.path.join(dp, fn).replace('\\', '/')
            for line, lit in scan(p):
                rows.append((p[len(ROOT) + 1:], line, lit))
    print('=' * 78)
    print('%s  … %d 件' % (title, len(rows)))
    total += len(rows)
    by = {}
    for f, line, lit in rows:
        by.setdefault(f, []).append((line, lit))
    for f in sorted(by):
        print('  %s' % f)
        for line, lit in by[f]:
            t = lit if len(lit) <= 78 else lit[:75] + '..."'
            print('    %5d: %s' % (line, t))
print('=' * 78)
print('合計 %d 件' % total)
