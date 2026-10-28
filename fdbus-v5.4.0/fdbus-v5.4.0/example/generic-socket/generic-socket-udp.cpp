/*
 * Copyright (C) 2015   Jeremy Chen jeremy_cz@yahoo.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define FDB_LOG_TAG "GSOCKET_TEST"
#include <fdbus/fdbus.h>
#include <iostream>

using namespace ipc::fdbus;
bool fdb_is_server = false;

class MyUdpSocket : public CGenericUdpSocket
{
protected:
    virtual void onPeerInput(FdbScktSessionPtr &session)
    {
        uint8_t input_data[1024];
        uint32_t peer_ip;
        int32_t peer_port;
        if (session->getSocket()->recv(input_data, sizeof(input_data), peer_ip, peer_port) < 0)
        {
            std::cout << "UDP onPeerInput: " << "error when read!" << std::endl;
        }
        else
        {
            std::cout << "UDP onPeerInput: " << (char *)input_data <<
                ", ip: " << peer_ip << ", port: " << peer_port << std::endl;

            if (fdb_is_server)
            {
                const char *data = "hello, peer! I'm reply to udp.";
                if (session->sendAsync((const uint8_t *)data, (int32_t)strlen(data) + 1, peer_ip, peer_port) <= 0)
                {
                    std::cout << "error when reply!" << std::endl;
                }
            }
        }
    }
    virtual void onPeerOutput(FdbScktSessionPtr &session)
    {}
    virtual void onPeerOnline(FdbScktSessionPtr &session)
    {
        std::cout << "UDP onPeerOnline!" << std::endl;
    }
    virtual void onPeerOffline(FdbScktSessionPtr &session)
    {
        std::cout << "UDP onPeerOffline!" << std::endl;
    }
};

static CBaseWorker my_worker("udp", FDB_WORKER_ENABLE_FD_LOOP);

int main(int argc, char **argv)
{
#ifdef __WIN32__
        WORD wVersionRequested;
        WSADATA wsaData;
        int err;
    
        /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
        wVersionRequested = MAKEWORD(2, 2);
    
        err = WSAStartup(wVersionRequested, &wsaData);
        if (err != 0)
        {
            /* Tell the user that we could not find a usable */
            /* Winsock DLL.                                  */
            printf("WSAStartup failed with error: %d\n", err);
        }
#endif

    if (argc < 3)
    {
        std::cout << "usage: generic-socket-udp peer-url self-url" << std::endl;
        std::cout << "usage: generic-socket-udp none self-url" << std::endl;
        std::cout << "usage: generic-socket-udp peer-url none" << std::endl;
        exit(0);
    }
    std::string peer_url = argv[1];
    std::string self_url = argv[2];
    if (peer_url == "none")
    {
        peer_url.clear();
    }
    if (self_url == "none")
    {
        self_url.clear();
    }

    my_worker.start();

    uint32_t peer_ip = 0;
    int32_t peer_port = -1;
    if (!peer_url.empty())
    {
        CFdbSocketAddr addr;
        if (!CBaseSocketFactory::parseUrl(peer_url.c_str(), addr))
        {
            std::cout << "Bad url: " << peer_url << std::endl;
            exit(-1);
        }
        if (!CBaseSocketFactory::parseIp(addr.mAddr.c_str(), peer_ip))
        {
            std::cout << "Bad url: " << peer_url << std::endl;
            exit(-1);
        }
        peer_port = addr.mPort;
    }

    fdb_is_server = peer_url.empty();
    MyUdpSocket udp_socket;
    FdbScktSessionPtr session = udp_socket.bind(self_url.c_str(), &my_worker);
    while (1)
    {
        while (FdbSessionDropped(session))
        {
            std::cout << "UDP: disconnected!" << std::endl;
            sysdep_sleep(500);
        }
        if (peer_port >= 0)
        {
            const char *data = "hello, peer! I'm udp.";
            if (!session->sendAsync((const uint8_t *)data, (int32_t)strlen(data) + 1, peer_ip, peer_port))
            {
                std::cout << "UDP: fail to send!" << std::endl;
            }
        }
        sysdep_sleep(500);
    }
}
