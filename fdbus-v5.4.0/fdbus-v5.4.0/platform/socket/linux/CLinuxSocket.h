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

#ifndef _CLINUXSOCKET_H_
#define _CLINUXSOCKET_H_

#include <fdbus/CSocketImp.h>
#include <platform/socket/sckt-0.5/sckt.hpp>

namespace ipc {
namespace fdbus {
struct CFdbInterfaceTable;
bool getIpAddressLinux(CFdbInterfaceTable &addr_tbl);
bool sameSubnetLinux(const char *ip1, const char *ip2, const char *mask);
bool isValidIpAddrLinux(const char *addr);

class CTCPTransportSocket : public CSocketImp
{
public:
    CTCPTransportSocket(sckt::TCPSocket *imp, EFdbSocketType type);
    ~CTCPTransportSocket();
    int32_t send(const uint8_t *data, int32_t size);
    int32_t send(const uint8_t *data, int32_t size, tFdbIpV4 ip, int32_t port);
    int32_t recv(uint8_t *data, int32_t size);
    int32_t recv(uint8_t *data, int32_t size, tFdbIpV4 &ip, int32_t &port);
    int getFd();
    bool createSSL(void *ctx, bool server);
    bool isSecure()
    {
        return !!mSSL;
    }
private:
    //sckt::Socket *mSocketImp;
    sckt::TCPSocket *mSocketImp;
    void *mSSL;
    int32_t sslSend(const uint8_t *data, int32_t size);
    int32_t sslRecv(uint8_t *data, int32_t size);
#ifdef CONFIG_SSL
    void showCerts();
#endif
};

class CLinuxClientSocket : public CClientSocketImp
{
public:
    CLinuxClientSocket(CFdbSocketAddr &addr);
    ~CLinuxClientSocket();
    CSocketImp *connect(const CSocketParams &params);
};

class CLinuxServerSocket : public CServerSocketImp
{
public:
    CLinuxServerSocket(CFdbSocketAddr &addr);
    ~CLinuxServerSocket();
    bool bind();
    CSocketImp *accept(const CSocketParams &params);
    int getFd();
private:
    sckt::TCPServerSocket *mServerSocketImp;
};

class CUDPTransportSocket : public CSocketImp
{
public:
    CUDPTransportSocket(sckt::UDPSocket *imp);
    ~CUDPTransportSocket();
    int32_t send(const uint8_t *data, int32_t size);
    int32_t send(const uint8_t *data, int32_t size, tFdbIpV4 ip, int32_t port);
    int32_t recv(uint8_t *data, int32_t size);
    int32_t recv(uint8_t *data, int32_t size, tFdbIpV4 &ip, int32_t &port);
    int getFd();
    bool setUDPDestAddr(const char *ip_addr, int32_t port);
    void getUDPDestAddr(tFdbIpV4 &peer_ip, int32_t &peer_port) const
    {
        peer_ip = mPeerUDPIp;
        peer_port = mPeerUDPPort;
    }
    const std::string &getUDPDestAddr(int32_t &port) const
    {
        port = mPeerUDPPort;
        return mPeerUDPIpStr;
    }
private:
    sckt::UDPSocket *mSocketImp;

    tFdbIpV4 mPeerUDPIp;
    int32_t mPeerUDPPort;
    std::string mPeerUDPIpStr;
};

class CLinuxUDPSocket : public CUDPSocketImp
{
public:
    CLinuxUDPSocket(CFdbSocketAddr &addr);
    CLinuxUDPSocket();
    CSocketImp *bind();
};
}
}
#endif
