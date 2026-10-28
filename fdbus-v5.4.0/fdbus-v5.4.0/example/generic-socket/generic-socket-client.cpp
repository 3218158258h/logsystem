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

FdbScktSessionPtr client_session;
class MyClientSocket : public CGenericClientSocket
{
protected:
    virtual void onPeerInput(FdbScktSessionPtr &session)
    {
        uint8_t input_data[1024];
        if (session->recvAsync(input_data, sizeof(input_data)) < 0)
        {
            session->fatalError(true);
            std::cout << "client onPeerInput: " << "error when read!" << std::endl;
        }
        else
        {
            std::cout << "client onPeerInput: " << (char *)input_data << std::endl;
        }
    }
    virtual void onPeerOutput(FdbScktSessionPtr &session)
    {}
    virtual void onPeerOnline(FdbScktSessionPtr &session)
    {
        client_session = session;
        std::cout << "client onPeerOnline!" << std::endl;
    }
    virtual void onPeerOffline(FdbScktSessionPtr &session)
    {
        CGenericClientSocket::onPeerOffline(session);
        std::cout << "client onPeerOffline!" << std::endl;
    }
};

static CBaseWorker my_worker("client", FDB_WORKER_ENABLE_FD_LOOP);

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

    if (argc < 2)
    {
        std::cout << "usage: generic-socket-client url" << std::endl;
        exit(0);
    }
    my_worker.start();
    MyClientSocket client_socket;
    client_socket.enableReconnect(true);
    client_session = client_socket.connect((const char *)argv[1], &my_worker);
    while (1)
    {
        if (FdbSessionDropped(client_session))
        {
            std::cout << "client: fail to connect!" << std::endl;
        }
        else
        {
            const char *data = "hello, server! I'm client.";
            if (!client_session->sendAsync((const uint8_t *)data, (int32_t)strlen(data) + 1))
            {
                std::cout << "client: fail to send!" << std::endl;
            }
        }
        sysdep_sleep(500);
    }
}
