# 偽 CCAPI カメラ(検証用)

エッジ端末に「カメラが N 台居る」と思わせ、**実機のカメラ無しで**台数を増やした
ときの内部RAM/挙動を測るための道具一式。

## なぜ作れるか

エッジは SSDP(M-SEARCH, `ST: ssdp:all`)の応答から `LOCATION` を拾い、UPnP
ディスクリプタ XML の `<ns:X_accessURL>` を **そのまま** CCAPI の土台にする
(`apiCanonCCAPI.cpp:242`)。host:port はこちらが自由に決められるので、
IP を増やせなくても **1つの IP でポートを変えれば別カメラとして扱わせられる**。

- カメラ n: ディスクリプタ = `49200+n` / CCAPI = `8100+n`
- ディスクリプタを **49152 に置かない**のは意図的。エッジの IP 直結経路
  (`detectCanonCCapi::identifyAt`)は :49152 決め打ちなので、そこに置くと
  全台が同じ1台に見えてしまう。空けておけば必ず SSDP 経路を通る。

## 使い方

```
python make_spec.py --count 8 --push    # 所持カメラを8台に増やしスマホへ反映 + spec.json
python fake_ccapi.py --count 8 --host 192.168.1.4 --spec spec.json
python selftest.py 192.168.1.4          # 応答がコード側の期待を満たすか確認
python measure.py --cams 8 --minutes 6  # 偽カメラ起動→計画投入→RAM 計測まで一気に
```

- `etp_client.py` … PC から ETP で計画送信/開始/停止(スマホ UI を介さない)
- `make_plan.py`  … 既存計画を土台にパノラマ計画を生成
- `dumplog.py`    … エッジのログ末尾をシリアル `t` で吸い出す
- `make_spec.py --restore --push` … 足した所持カメラを片付ける

エッジは **STA モード**で PC と同じ LAN に居ること(`S` で切替、`A` で AP へ戻す)。
STA の接続先が壊れていたら `W` でファーム既定へ戻せる。

## 忠実でない点(結果を読むときの注意)

- 全台が同じ IP。エッジが serial→IP を覚える経路では全台が同じ IP を指す。
- 露光を待たない(シャッター POST は即 200)。実機の busy は再現しない。
- SSDP 応答は間隔を空けて返す(`--ssdp-gap`)。一気に投げると受信側が取りこぼす。

## これで見つかったこと(2026-08-25)

- **HTTP接続スロットの取り違え**: `setReuse(true)` のまま `end()` してもソケットが
  残り、別の host:port へ `begin()` すると前の接続へ要求が飛ぶ。枠は3本しかないので
  4宛先目から別カメラの応答が返っていた。`net.cpp` の退避時に `setReuse(false)` を
  入れて解消(8台すべて確立を確認)。
- **内部RAM**: 8台確立で 93,372 → 60,848(約32.5KB消費)、水位(min)は 16,340 まで低下。
