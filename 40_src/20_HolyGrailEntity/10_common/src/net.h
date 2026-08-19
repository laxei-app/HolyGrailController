#ifndef _NET_H_
#define _NET_H_

#include <string>
#include <vector>

namespace net 
{

    bool init();
    bool deInit();

    // --- 追加: 利用可能なローカルIPアドレスのリストを返す ---
    std::vector<std::string> getLocalIpList();

    // 探索の手がかりになる近傍ホストのIP。自分がDHCPを配っているときはその貸出先を返す。
    //  そうでなければ空。**呼ぶ側はモードを知らなくてよい**(空なら何も足さないだけ)。
    std::vector<std::string> neighborHostIps();

    // 限定サブネットのバッチ探索(§3.3 tier3)。自ホストのIP+ネットマスクから探索範囲(同一サブネットの
    // ホスト部)を割り出し、指定ポートが開いているホストのIP一覧を返す。非ブロッキング connect を
    // バッチ並行で行い、応答(接続成立)したIPのみを返す(=生存かつ :port サービス有り)。
    //  timeoutMs: 1バッチあたりの待ち時間[ms]。maxHosts: 探索する最大ホスト数(コスト上限)。
    // 前回IP直結(tier1)・M-SEARCH(tier2)が不発の時の最終手段。エッジ(lwIP)のみ実装、他は空を返す。
    std::vector<std::string> scanSubnetPort(int port, int timeoutMs, int maxHosts);

    // SSDP関連(能動: M-SEARCH送信→応答受信)
    void* ssdpStart(const std::string& query, const std::string& localIp = "");
    bool ssdpRead(void* handle, std::string& answer);
    void ssdpClose(void* handle);

    // SSDP受動待ち受け(NOTIFY受信)。0.0.0.0:1900 に bind し 239.255.255.250 へ参加する。
    // M-SEARCH は送らず、カメラが自発広告する NOTIFY を受ける(スマホ接続後などにSSDP広告を
    // 止めるカメラの復帰は拾えないが、電源復帰・再起動時の再出現を60秒待たず即検知するため)。
    // 専用ソケット+専用スレッドから使い、単一 netThread ワーカーは迂回する。
    //  ※Android は Java 側 WifiManager.MulticastLock 保持 + 権限 CHANGE_WIFI_MULTICAST_STATE が必要
    //    (無いと Wi-Fi ドライバがマルチキャスト受信を黙って破棄する)。Windows はスタブ(非対象)。
    void* ssdpListenStart(void);
    bool  ssdpListenRead(void* handle, std::string& answer);
    void  ssdpListenClose(void* handle);

    // --- HTTPのタイムアウト(2026-08-06) ---
    // エッジとスマホで同じ値を使う。片方だけ変えると、同じ共通ロジック(captureRunner等)が
    // 端末によって違う待ち方をすることになり、片方でだけ再現する不具合を生む。
    // 実際、以前は GET だけ 500ms で PUT/POST は未設定(Arduino既定の5000ms)という食い違いがあり、
    // 「露出設定の2秒リトライが1回も再試行できていない」原因になっていた。
    //  接続: 相手が応答を返し始めるまで待つ。下の実測を参照。
    //  送受信: 応答が返らない相手を待ち続けない。実測の最長は数百ms。
    //
    // 【接続を 1500ms から 6000ms へ広げた(2026-08-14)】
    //  EOS R50 V が「見つかっているのに繋がらない」状態になり、原因がこの値だった。
    //  スマホ本体から15回ずつ実測した TCP 接続だけの所要時間[ms]:
    //    EOS R100 : min 50 / 中央 70 / max 90            ← 安定
    //    EOS R50 V: min 80 / 中央 710 / max 5050         ← 60倍以上ばらつく
    //      生データ 3140 5050 710 500 480 80 80 1550 80 1520 1150 1370 470 500 1140
    //  1500ms では 15回中 7回(47%)が超過し、errno=110 で弾かれていた。値が 1.5秒刻みに
    //  山を作るので、最初の SYN が落ちて再送で拾われている(=無線の省電力)と見られる。
    //  接続さえ張れれば要求と応答は数十msで終わる(接続のみ と 接続+要求+応答 がほぼ同値)
    //  ため、伸ばすのは接続だけでよく、送受信の 3000ms は据え置く。
    //
    // 【探索が遅くならない理由】「居ないIPを待たない」役目はこの値ではなく
    //  net::scanSubnetPort(port, 250ms, ...) が持っている。サブネット探索は先に 250ms の
    //  ポートスキャンで生きているホストだけに絞り、そのあとで初めて HTTP を張る。
    //  したがってここを広げても、探索が空振りで待たされることはない。
    constexpr int kHttpConnectTimeoutMs = 6000;
    constexpr int kHttpIoTimeoutMs      = 3000;

    // HTTP関連
    void httpBreak(void);
    bool httpGet(const std::string& url, std::string& answer);
    // 範囲指定GET(HTTP Range)。大きなファイルの先頭だけ要るときに使う。
    //  用途: 撮影画像のEXIFからセンサー寸法/画素数を読む(先頭64KBで足りる。本体は
    //  数MB〜数十MBあるので全部落とすわけにいかない)。
    //  戻り: 206(部分)か200(相手がRange非対応で全部返した)なら true。
    bool httpGetRange(const std::string& url, long from, long to, std::string& answer);
    bool httpPost(const std::string& url, const std::string& body, std::string& response);
    bool httpPut(const std::string& url, const std::string& body, std::string& response);
    bool httpDelete(const std::string& url, std::string& response);

    // 直前の http*() が受け取った HTTP ステータスコードを返す(同一スレッドで直後に読むこと)。
    //  0 = 応答なし(接続失敗/送信失敗/応答を解釈できず)。それ以外は受信したステータス。
    // 失敗が「カメラが 503 等で断ったのか」「そもそも届かなかったのか」をログで区別するために使う。
    // 2026-07-21: actShutter 失敗(code=3)の原因を後から特定できず、この区別が必要と判明した。
    int  lastHttpStatus(void);

 
};

#endif  // _NET_H_