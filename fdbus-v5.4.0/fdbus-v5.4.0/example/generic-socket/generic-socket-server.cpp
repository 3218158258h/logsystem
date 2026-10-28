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
#include <string.h>
#include <fdbus/fdbus.h>
#include <iostream>

using namespace ipc::fdbus;
class MyServerSocket : public CGenericServerSocket
{
protected:
    virtual void onPeerInput(FdbScktSessionPtr &session)
    {
        uint8_t input_data[1024];
        if (session->recvAsync(input_data, sizeof(input_data)) < 0)
        {
            session->fatalError(true);
            std::cout << "server onPeerInput: " << "error when read!" << std::endl;
        }
        else
        {
            std::cout << "server onPeerInput: " << (char *)input_data << std::endl;
        }
    }
    virtual void onPeerOutput(FdbScktSessionPtr &session)
    {}
    virtual void onPeerOnline(FdbScktSessionPtr &session)
    {
        std::cout << "server onPeerOnline!" << std::endl;
    }
    virtual void onPeerOffline(FdbScktSessionPtr &session)
    {
        std::cout << "server onPeerOffline!" << std::endl;
    }
};

static CBaseWorker my_worker("server", FDB_WORKER_ENABLE_FD_LOOP);

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
        std::cout << "usage: generic-socket-server url" << std::endl;
        exit(0);
    }
    my_worker.start();
    MyServerSocket server_socket;
    if (!server_socket.bind((const char *)argv[1], &my_worker))
    {
        std::cout << "server: unable to bind to " << argv[1] << std::endl;
        exit(-1);
    }
    while (1)
    {
        CGenericServerSocket::tSessionContainer sessions;
        server_socket.getConnectedSessions(sessions);
        for (auto it = sessions.begin(); it != sessions.end(); ++it)
        {
            auto &session = *it;
            if (FdbSessionDropped(session))
            {
                std::cout << "server: disconnected!" << std::endl;
                continue;
            }
            const char *data = "hello, server! I'm client.";
            if (!session->sendAsync((const uint8_t *)data, strlen(data) + 1))
            {
                std::cout << "server: fail to send!" << std::endl;
            }
            sysdep_sleep(500);
        }
    }
}
