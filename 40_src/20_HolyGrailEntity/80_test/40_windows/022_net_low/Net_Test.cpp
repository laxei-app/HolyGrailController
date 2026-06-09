// Net_Test.cpp : このファイルには 'main' 関数が含まれています。プログラム実行の開始と終了がそこで行われます。
//

#include <iostream>
#include "net.h"

int main()
{
    // 1. 親クラスのポインタに、Windows用の子クラスを生成して入れる
    class net* net = new class net();

    // 2. NICのリストを取得してみる
    std::vector<std::string> ips = net->getLocalIpList();

    for (const auto& ip : ips) {
        printf("Found NIC: %s\n", ip.c_str());
        std::string query = 
            "M-SEARCH * HTTP/1.1\r\n"
            "HOST: 239.255.255.250:1900\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "ST: ssdp:all\r\n"
            "MX: 2\r\n\r\n";
        
        void * handle = net->ssdpStart(query, ip.c_str());

        std::string deviceInfo;
        do
        {
            deviceInfo = net->ssdpRead(handle);
            if (deviceInfo.length() == 0) { break; }
            printf("device ===================\r\n%s",deviceInfo.c_str());
        } while (deviceInfo.length() != 0);
        net->ssdpClose(handle);
    }

    // 3. 使い終わったらメモリを解放
    delete net;
}

