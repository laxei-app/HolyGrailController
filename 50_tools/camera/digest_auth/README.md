# ダイジェスト認証(CCAPI)の検証ツール

2026-08-15 に EOS R50 V で切り分けたときのもの。カメラが無くても回帰できるように残す。

| ファイル | 用途 |
|---|---|
| `mockcam.py` | **カメラの偽物**。実機と同じ形のチャレンジを返し、`nc` の逆順/使用済みをリプレイとして弾き、認証失敗が続くと 403 `{"message":"Not access"}` で締め出す。`python mockcam.py 8099 <user> <pass>` |
| `ncprobe.py` | 実機の `nc` 受理条件を測る。意図的な認証失敗は1回だけ。`python ncprobe.py <ip> <user> <pass>`(資格情報はコード内に書かない) |
| `tseq.cpp` | 負荷試験。第1相=1スレッド逐次、第2相=2スレッド並行。`tseq <ip> <port> <user> <pass> [第1相秒] [noguard]` |
| `tlive.cpp` | 実機に1発だけ投げて 401→認証→200 を見る |
| `tnc.cpp` `tauth.cpp` `treal.cpp` | httpAuth の単体テスト(nc/錠/チャレンジ解釈) |

## ビルド

```
cl /nologo /EHsc /std:c++17 /utf-8 /wd4819 ^
   /I ..\..\..\40_src\20_HolyGrailEntity\10_common\src ^
   tseq.cpp ..\..\..\40_src\20_HolyGrailEntity\10_common\src\httpAuth.cpp ^
            ..\..\..\40_src\20_HolyGrailEntity\10_common\src\md5.cpp /Fe:tseq.exe
```

## 実測して分かったカメラの受理条件

- nonce を使い回し、`nc` を**接続とプロセスをまたいで**覚えている
- その nonce で見た最大値より大きければ通る。**飛びは自由**（`nc=2` の直後に `nc=100000` が通り、その後 `nc=50` は弾かれた）
- 認証失敗が続くと 403 で締め出す。**カメラ本体でユーザー認証を入れ直すまで戻らない**

## 注意

`mockcam.py` はローカルホストだと速すぎて `nc` の逆順が起きない。`tseq.cpp` は接続時間の
ばらつき（実機で 80〜5000ms）を模した待ちを自前で入れている。ここを消すと再現しない。
