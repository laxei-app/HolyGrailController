#include "common.h"
#include "net.h"
#include "netThread.h"
#include "osSystemCall.h"
#include <mutex>
#include <deque>
#include <condition_variable>
#include <future>
#include <vector>

namespace netThread
{

    enum class queType
    {
        LOCAL_IP,
        SSDP_START,
        SSDP_READ,
        SSDP_CLOSE,
        HTTP_GET,
        HTTP_POST,
        HTTP_PUT,
        HTTP_DELETE,
        FINISH
    };

    // 処理内容を表す構造体。queType によって内容が変わる。
    struct requestQue_t
    {
        queType             type;
        std::promise<bool>  completion;

        requestQue_t() : type(queType::SSDP_START) {}
		requestQue_t(const requestQue_t& other) : type(other.type) {}
        requestQue_t& operator=(const requestQue_t& other) 
        {
            if (this != &other) {
                type = other.type;
            }
            return *this;
		}

        virtual ~requestQue_t(){};
    };
    
    namespace
    {
        std::mutex mtx;                         // ミューテックス
        std::condition_variable cv_request;     // 条件変数
        std::deque< requestQue_t*> que_;  // 処理内容を表す構造体のリスト
		bool stopThread = false;                // スレッド停止フラグ(全ワーカー共通)
        // Phase4(1エッジ複数カメラ同時): 単一ワーカーだと2台目の測光/ウォームアップが
        // 先行カメラのHTTPに阻まれて進まないため、同一キューを引く並行ワーカープールにする。
        // スマホ/エッジ共通コード。net層は各プラットフォームとも呼び出し毎に独立ソケット/HTTPClientを
        // 生成する実装なので並行呼び出しに対して安全。ワーカー数=2カメラ同時+発見/keepAliveの余裕1。
        const int          kWorkerCount = 3;
        std::vector<void*> workers_;            // ワーカースレッドのハンドル一覧
    }


    // 処理内容を表す構造体。
    struct local_ip_t : public requestQue_t
    {
		std::vector<std::string>& ipList;

        // コンストラクタ
		local_ip_t() : ipList(*(new std::vector<std::string>()))
        {
            type = queType::LOCAL_IP;
        }
        local_ip_t(const local_ip_t& other)
            : requestQue_t(other), ipList(other.ipList) {
        }
        ~local_ip_t() {delete &ipList;  }

    };

	// ssdpStartの要求内容を表す構造体。
	struct ssdp_start_t : public requestQue_t
    {
        const std::string&         query;
        const std::string&         localIp;
        void*                       handle;

		// コンストラクタ
        ssdp_start_t(const std::string& query, const std::string& localIp )
        // 修正: ssdp_start 構造体の参照メンバーの初期化子リスト追加
        : query(query), localIp(localIp), handle(nullptr)
        {
            type = queType::SSDP_START;
        }
		ssdp_start_t(const ssdp_start_t& other)
        : requestQue_t(other), query(other.query), localIp(other.localIp), handle(other.handle) {}
        ~ssdp_start_t() {}

    };

    struct ssdp_read_t : public requestQue_t
    {
        void*               handle;
        std::string&        answer;
        bool                result;

        // コンストラクタ
        ssdp_read_t(void * handle, std::string& answerRef)
            : handle(handle), answer(answerRef)
        {
            type = queType::SSDP_READ;
            result = false;
        }
        ssdp_read_t(const ssdp_read_t& other) : requestQue_t(other), handle(other.handle), answer(other.answer) 
        {
			result = other.result;
        }
        ~ssdp_read_t() {}
    };

    struct ssdp_close_t : public requestQue_t
    {
        void* handle;

		// コンストラクタ
        ssdp_close_t(void * handle)
        {
            type = queType::SSDP_CLOSE;
            this->handle = handle;
        }
        ssdp_close_t(const ssdp_close_t& other) : requestQue_t(other), handle(other.handle) {}
        ~ssdp_close_t() {}
    };

    struct http_t : public requestQue_t
    {
        const std::string& url;
        const std::string& body;
        std::string&        response;
		bool                result;

		// コンストラクタ
        http_t(const std::string& url, const std::string& body, std::string& response) : url(url), body(body), response(response)
        {
            type = queType::HTTP_GET; // デフォルトは GET とする
            result = false;
		}
        http_t(http_t& other) : requestQue_t(other), url(other.url), body(other.body), response(other.response) 
        {
            result = false;
        }
		~http_t() {}
    };
    
    // スレッドに要求しその終了を待つ
    // return : 実行結果
    errCode execRequest(requestQue_t* req)
    {
        std::promise<bool> prom;
        std::future<bool> fut = prom.get_future();

		{   // mutex のスコープを限定するためにブロックを作成
            std::lock_guard<std::mutex> lock(mtx);
			req->completion = std::move(prom);   // promise をリクエストにセット
			que_.push_back(req);                // que_ にリクエストを追加
        }
        cv_request.notify_one();                // 要求をスレッドに通知
        try
        {
            bool success = fut.get(); // 結果を取得
            if (!success)
            {
                return ERR_HGC_NET_THREAD; // スレッド内での処理失敗
            }
        }
        catch (const std::exception& e)
        {
            DBGLN(col::RED, "Exception in execRequest: %s", e.what());
            return ERR_HGC_NET_THREAD; // スレッド内での例外発生
		}
        return ERR_HGC_OK;          // 成功
    }

    /////////////////////////////////////////////////////
    // 利用可能なローカルIPアドレスのリストを返す
    std::vector<std::string> getLocalIpList()
    {
        local_ip_t req;
        auto result = execRequest(&req);            // 実行
        return req.ipList;                          // 成功
    }

    // AP接続局IP一覧: 軽量な local 問い合わせなのでワーカーキューを通さず直接呼ぶ。
    std::vector<std::string> apClientIps()
    {
        return net::apClientIps();
    }

    /////////////////////////////////////////////////////////
    // SSDP関連
    
	// SSDP の開始。
	// return : ssdpReadで使用するためのハンドル。失敗した場合は nullptr を返す。
    void* ssdpStart(const std::string& query, const std::string& localIp)
    {
        ssdp_start_t req(query, localIp);             // 要求の作成
        auto result = execRequest(&req);            // 実行
        if (result != ERR_HGC_OK) { return nullptr; }
        return req.handle;                          // 成功
    }

    // SSDP の開始。
    // return : ssdpReadで使用するためのハンドル。失敗した場合は nullptr を返す。
    bool ssdpRead(void* handle, std::string& answer)
    {
		ssdp_read_t req(handle, answer);
        auto result = execRequest(&req);        // 実行
        if (result != ERR_HGC_OK) { return false; }
        return req.result;                          // 成功
    }

    void ssdpClose(void* handle )
    {
        ssdp_close_t req(handle);
        execRequest(&req);                          // 実行
    }

    ////////////////////////////////////////////////////////////
    // HTTP関連
    void httpBreak(void)
    {

    }

	// HTTP GET
	// return : 成功した場合は true、失敗した場合は false を返す。
    bool httpGet(const std::string& url, std::string& answer)
    {
		std::string dummyBody; // GET なので body は空
        http_t req(url, dummyBody, answer);
        req.type = queType::HTTP_GET;
        auto result = execRequest(&req);            // 実行
        if (result != ERR_HGC_OK) { return false; }
        return req.result;                          // 成功
    }

	// HTTP POST
	// return : 成功した場合は true、失敗した場合は false を返す。
    bool httpPost(const std::string& url, const std::string& body, std::string& response )
    {
        http_t req(url, body, response);
        req.type = queType::HTTP_POST;
        auto result = execRequest(&req);            // 実行
        if (result != ERR_HGC_OK) { return false; }
        return req.result;                          // 成功
    }

	// HTTP PUT
	// return : 成功した場合は true、失敗した場合は false を返す。
    bool httpPut(const std::string& url, const std::string& body, std::string& response )
    {
        http_t req(url, body, response);
        req.type = queType::HTTP_PUT;
        auto result = execRequest(&req);            // 実行
        if (result != ERR_HGC_OK) { return false; }
        return req.result;                          // 成功
    }

	// HTTP DELETE
	// return : 成功した場合は true、失敗した場合は false を返す。
    bool httpDelete(const std::string& url, std::string& response)
    {
		std::string dummyBody; // DELETE なので body は空
        http_t req(url, dummyBody, response);
        req.type = queType::HTTP_DELETE;
        auto result = execRequest(&req);            // 実行
        if (result != ERR_HGC_OK) { return false; }
        return req.result;                          // 成功
    }

    //////////////////////////////////////////////////////////////////////////
    // スレッド
	static errCode threadFunc(void* param);

    // スレッドの開始と終了
    void init(void)
    {
        net::init();            // net の初期化(ワーカー起動前に1回)
        stopThread = false;
        workers_.clear();
        for (int i = 0; i < kWorkerCount; ++i)
        {
            workers_.push_back(ossc::threadNet(threadFunc, nullptr));
        }
    }

	// スレッドの終了
    void deInit(void)
    {
        { std::lock_guard<std::mutex> lock(mtx); stopThread = true; }
        cv_request.notify_all();                    // 全ワーカーを起こして終了させる
        for (void* h : workers_) { ossc::threadEnd(h); }
        workers_.clear();
        net::deInit();          // net の終了(全ワーカー停止後に1回)
    }

    // スレッド本体
    errCode threadFunc(void* param)
    {
		http_t* httpReq = nullptr;
		ssdp_start_t* ssdpStartReq = nullptr;
		ssdp_read_t* ssdpReadReq = nullptr;
		ssdp_close_t* ssdpCloseReq = nullptr;
		local_ip_t* localIpReq = nullptr;
        // net::init/deInit は init()/deInit() で1回ずつ行う(複数ワーカーで多重初期化しない)。

        while (true)
        {
            requestQue_t* req = nullptr;
			{   // mutex のスコープを限定するためにブロックを作成
                std::unique_lock<std::mutex> lock(mtx);

                cv_request.wait(lock, [] { return !que_.empty() || stopThread; });
                if (stopThread && que_.empty()) { break; }	// 停止要求 & キュー空 → このワーカー終了
                req = que_.front();
                que_.pop_front();
            }

            switch (req->type)
            {
            case queType::LOCAL_IP:
                localIpReq = static_cast<local_ip_t*>(req);
                localIpReq->ipList = net::getLocalIpList();
				break;

            case queType::SSDP_START:
                ssdpStartReq = static_cast<ssdp_start_t*>(req);
                ssdpStartReq->handle = net::ssdpStart(ssdpStartReq->query, ssdpStartReq->localIp);
                break;

            case queType::SSDP_READ:
                ssdpReadReq = static_cast<ssdp_read_t*>(req);
                ssdpReadReq->result= net::ssdpRead(ssdpReadReq->handle, ssdpReadReq->answer);
                break;

            case queType::SSDP_CLOSE:
                ssdpCloseReq = static_cast<ssdp_close_t*>(req);
                net::ssdpClose(ssdpCloseReq->handle);
                break;

            case queType::HTTP_GET:
                httpReq = static_cast<http_t*>(req);
                httpReq->result = net::httpGet(httpReq->url, httpReq->response);
                break;

            case queType::HTTP_POST:
                httpReq = static_cast<http_t*>(req);
                httpReq->result = net::httpPost(httpReq->url, httpReq->body, httpReq->response);
                break;

            case queType::HTTP_PUT:
                httpReq = static_cast<http_t*>(req);
                httpReq->result = net::httpPut(httpReq->url, httpReq->body, httpReq->response);
                break;

            case queType::HTTP_DELETE:
                httpReq = static_cast<http_t*>(req);
                httpReq->result = net::httpDelete(httpReq->url, httpReq->response);
                break;

            case queType::FINISH:
                stopThread = true;		// 旧経路の互換(現在は deInit の stopThread+notify_all で停止)
                break;
            }
			req->completion.set_value(true);    // 処理の完了を通知
        }
        return ERR_HGC_OK;
    }
}