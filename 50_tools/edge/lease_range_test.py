#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DHCP の配布開始位置の決め方を確かめる(edgeApLeases::chooseStartOctet の写し)。

なぜ写しなのか
--------------
本体は 40_src/10_UI/18_M5Common/edgeApLeases.h にあるが、Arduino/lwIP のヘッダを
掴んでいるので Windows の cl.exe では素直に組めない。ここでは**手順だけ**を写して、
台数と歯抜けの組み合わせを総当たりに近い形で確かめる。
本体を直したらこちらも直すこと(定数は下の3つだけ)。

確かめたいこと
--------------
・記録に載っている IP を1つも含まない、**連続した空き**の一番下を選ぶ
・カメラの電源を何度入り切りしても、配る範囲が既に使われている IP とぶつからない
・戻ってこないカメラが増えても(行が残っても)ぶつからない

  python lease_range_test.py
"""

# edgeApLeases.h の kPoolWidth / kOctetLo / kOctetHi と一致させる
W, LO, HI = 11, 2, 244


def choose(held):
    """記録(held=埋まっている最終オクテットの集まり)から配布開始位置を決める。"""
    used = [False] * 256
    used[0] = used[1] = used[255] = True     # ネットワーク / AP自身 / ブロードキャスト
    for o in held:
        used[o] = True
    s = LO
    while s <= HI:
        hit = -1
        for k in range(W):
            if used[s + k]:
                hit = k
                break
        if hit < 0:
            return s
        s += hit + 1                          # 詰まっていた場所の1つ先から試し直す
    return LO                                 # /24 が埋まった(通常は起こらない)


def check(name, held, expect=None):
    r = choose(held)
    bad = set(range(r, r + W)) & set(held)
    assert not bad, (name, r, sorted(bad))
    assert LO <= r <= HI, (name, r)
    if expect is not None:
        assert r == expect, (name, r, expect)
    print("  OK  %-40s held=%-22s -> .%d (.%d-.%d)"
          % (name, sorted(held) if len(held) < 8 else "%d件" % len(held), r, r, r + W - 1))


def main():
    print("[1] 代表的な形")
    check("記録なし(初回起動)", [], LO)
    check("3台が .2-.4 を保持", [2, 3, 4], 5)
    check("4台のうち真ん中が抜けた", [2, 3, 5], 6)
    check("上に寄った後で下が空いた", [13, 14, 15], 2)
    check("歯抜けが幅ちょうど空く", [2, 14], 3)
    check("歯抜けが幅に1つ足りない", [2, 13], 14)
    check("遠くに1台だけ", [200], 2)
    check("下が埋まりきっている", list(range(2, 240)), 240)
    # 実機で起きた形: DHCPを引き直さないカメラ(.4)が混ざる
    check("引き直さないカメラを含む4台", [8, 9, 11, 4], 12)

    print("[2] 電源の入り切りを繰り返す(全台が戻る)")
    held = [2, 3, 4]
    for i in range(30):
        s = choose(held)
        assert not (set(range(s, s + W)) & set(held)), (i, s, held)
        held = [s, s + 1, s + 2]          # 戻ってきた3台が新しい範囲を貰う(古い行は MAC で置換)
    print("  OK  30回ぶつからない。最後の保持 = %s" % held)

    print("[3] 毎回1台が戻ってこない(行が残り続ける)")
    held, ghosts = [2, 3, 4], []
    for i in range(20):
        s = choose(held + ghosts)
        assert not (set(range(s, s + W)) & set(held + ghosts)), (i, s, held, ghosts)
        ghosts.append(held[0])
        held = [s, s + 1]
    print("  OK  20回ぶつからない。残骸 %d 件、現用 = %s" % (len(ghosts), held))

    print("[4] 総当たり(4台までの位置の組み合わせ)")
    import itertools
    n = 0
    for k in range(0, 5):
        for held in itertools.combinations(range(2, 40), k):
            s = choose(list(held))
            assert not (set(range(s, s + W)) & set(held)), (held, s)
            n += 1
    print("  OK  %d 通りすべてぶつからない" % n)

    print("\nすべて合格")


if __name__ == "__main__":
    main()
