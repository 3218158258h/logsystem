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

#include "CLinuxSocket.h"
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/CBaseSysDep.h>
#ifdef CONFIG_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#ifdef __WIN32__
#ifdef __cplusplus
extern "C" {
#endif
#include <openssl/applink.c>
#ifdef __cplusplus
}
#endif
#endif
#endif

namespace ipc {
namespace fdbus {
#define FDB_SSL_CONNECT_RETRY_NR    128
#define FDB_SSL_CONNECT_RETRY_INTERVAL 50

CTCPTransportSocket::CTCPTransportSocket(sckt::TCPSocket *imp, EFdbSocketType type)
    : mSocketImp(imp)
    , mSSL(0)
{
    mCred.pid = imp->pid;
    mCred.gid = imp->gid;
    mCred.uid = imp->uid;
    mConn.mPeerIp = imp->peer_ip;
    mConn.mPeerIpDigit = imp->peer_ip_digit;
    mConn.mPeerPort = imp->peer_port;

    // For TCP socket, address of CTCPTransportSocket is the different from CLinuxServerSocket
    // So you SHOULD get address either from session only!!!
    mConn.mSelfAddress.mType = type;
    mConn.mSelfAddress.mAddr = imp->self_ip;
    mConn.mSelfAddress.mAddrDigit = imp->self_ip_digit;
    mConn.mSelfAddress.mPort = imp->self_port;
}

CTCPTransportSocket::~CTCPTransportSocket()
{
    if (mSocketImp)
    {
        delete mSocketImp;
    }
#ifdef CONFIG_SSL
    if (mSSL)
    {
        SSL_free((SSL *)mSSL);
    }
#endif
}

int32_t CTCPTransportSocket::send(const uint8_t *data, int32_t size)
{
    int32_t ret = -1;
    if (mSocketImp)
    {
        try
        {
            if (mSSL)
            {
                ret = sslSend(data, size);
            }
            else
            {
                ret = mSocketImp->Send(data, size);
            }
        }
        catch(...)
        {
            ret = -1;
        }
    }
    return ret;
}

int32_t CTCPTransportSocket::send(const uint8_t *data, int32_t size, tFdbIpV4 ip, int32_t port)
{
    return send(data, size);
}

int32_t CTCPTransportSocket::recv(uint8_t *data, int32_t size)
{
    int32_t ret = -1;
    if (mSocketImp)
    {
        try
        {
            if (mSSL)
            {
                ret = sslRecv(data, size);
            }
            else
            {
                ret = mSocketImp->Recv(data, size);
            }
        }
        catch(...)
        {
            ret = -1;
        }
    }
    return ret;
}

int32_t CTCPTransportSocket::recv(uint8_t *data, int32_t size, tFdbIpV4 &ip, int32_t &port)
{
    if (mSocketImp)
    {
        ip = mSocketImp->peer_ip_digit;
        port = mSocketImp->peer_port;
    }
    return recv(data, size);
}

int CTCPTransportSocket::getFd()
{
    if (mSocketImp)
    {
        return mSocketImp->getNativeSocket();
    }
    return -1;
}

#ifdef CONFIG_SSL
enum ESSLStatus
{
    SSL_ST_COMPLETE,
    SSL_ST_CONTINUE,
    SSL_ST_ERROR
};

static ESSLStatus check_ssl_status(SSL *ssl, int32_t ret)
{
    ESSLStatus status;
    int32_t ssl_error = SSL_get_error(ssl, ret);
    switch(ssl_error)
    {
        case SSL_ERROR_NONE:
            // do our stuff with buffer_array here
            status = SSL_ST_COMPLETE;
        break;

        case SSL_ERROR_WANT_READ:
            // the operation did not complete, block the read
            // fall through
        case SSL_ERROR_WANT_WRITE:
            // the operation did not complete
            status = SSL_ST_CONTINUE;
        break;
        default:
            // some other error, clean up
            status = SSL_ST_ERROR;
        break;
    }

    return status;
}

void CTCPTransportSocket::showCerts()
{
    X509 * cert;
    char * line;
    cert = SSL_get_peer_certificate((SSL *)mSSL);
    if (cert != NULL)
    {
        printf("CA Info:\n");
        line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
        printf("CA Name: %s\n", line);
        free(line);
        line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
        printf("Issuer: %s\n", line);
        free(line);
        X509_free(cert);
    }
}

bool CTCPTransportSocket::createSSL(void *ctx, bool server)
{
    if (!ctx)
    {
        return false;
    }
    auto ssl = SSL_new((SSL_CTX *)ctx);
    if (ssl)
    {
        SSL_set_mode(ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        SSL_set_fd(ssl, mSocketImp->getNativeSocket());
        int32_t retries = FDB_SSL_CONNECT_RETRY_NR;
        ESSLStatus status;
        while (1)
        {
            int32_t ret;
            if (server)
            {
                ret = SSL_accept(ssl);
            }
            else
            {
                ret = SSL_connect(ssl);
            }
            retries--;
            status = check_ssl_status(ssl, ret);
            if ((status == SSL_ST_COMPLETE) || (status == SSL_ST_ERROR) || (retries <= 0))
            {
                break;
            }

            sysdep_sleep(FDB_SSL_CONNECT_RETRY_INTERVAL);
        }

        if (status != SSL_ST_COMPLETE)
        {
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            return false;
        }

        showCerts();

        if (mSSL)
        {
            SSL_free((SSL *)mSSL);
        }
        mSSL = ssl;
        return true;
    }
    return false;
}

int32_t CTCPTransportSocket::sslSend(const uint8_t *data, int32_t size)
{
    if (!mSSL)
    {
        return -1;
    }
    int32_t write_cnt = 0;
    auto bytes_written = SSL_write((SSL *)mSSL, data, size);
    auto status = check_ssl_status((SSL *)mSSL, bytes_written);
    if (status == SSL_ST_ERROR)
    {
        ERR_print_errors_fp(stdout);
        return -1;
    }

    if (bytes_written > 0)
    {
        write_cnt += bytes_written;
    }
    return write_cnt;
}

int32_t CTCPTransportSocket::sslRecv(uint8_t *data, int32_t size)
{
    if (!mSSL)
    {
        return -1;
    }
    ESSLStatus status;
    int32_t read_cnt = 0;
    do
    {
        auto bytes_read = SSL_read((SSL *)mSSL, data + read_cnt, size - read_cnt);
        status = check_ssl_status((SSL *)mSSL, bytes_read);
        if (bytes_read > 0)
        {
            read_cnt += (int32_t)bytes_read;
        }
    } while (SSL_pending((SSL *)mSSL) && (status != SSL_ST_ERROR) && (read_cnt < size));

    if (status == SSL_ST_ERROR)
    {
        ERR_print_errors_fp(stdout);
        return -1;
    }

    return read_cnt;
}
#else
bool CTCPTransportSocket::createSSL(void *ctx, bool server)
{
    return false;
}
int32_t CTCPTransportSocket::sslSend(const uint8_t *data, int32_t size)
{
    return -1;
}
int32_t CTCPTransportSocket::sslRecv(uint8_t *data, int32_t size)
{
    return -1;
}
#endif

CLinuxClientSocket::CLinuxClientSocket(CFdbSocketAddr &addr)
    : CClientSocketImp(addr)
{
}

CLinuxClientSocket::~CLinuxClientSocket()
{
}

CSocketImp *CLinuxClientSocket::connect(const CSocketParams &params)
{
    CSocketImp *ret = 0;
    sckt::Options opt(!params.mBlockMode, params.mKeepAliveInterval, params.mKeepAliveRetry);
    try
    {
        sckt::TCPSocket *sckt_imp = 0;
        if (mConn.mSelfAddress.mType == FDB_SOCKET_TCP)
        {
            if (mConn.mSelfAddress.mAddr.empty())
            {
                mConn.mSelfAddress.mAddr = "127.0.0.1";
            }
            sckt::IPAddress address(mConn.mSelfAddress.mAddr.c_str(), mConn.mSelfAddress.mPort);
            sckt_imp = new sckt::TCPSocket(address, &opt);
        }
#ifndef __WIN32__
        else if (mConn.mSelfAddress.mType == FDB_SOCKET_IPC)
        {
            sckt::IPAddress address(mConn.mSelfAddress.mAddr.c_str());
            sckt_imp = new sckt::TCPSocket(address, &opt);
        }
#endif

        if (sckt_imp)
        {
            ret = new CTCPTransportSocket(sckt_imp, mConn.mSelfAddress.mType);
            CFdbSocketConnInfo &conn_info = const_cast<CFdbSocketConnInfo &>(ret->getConnectionInfo());
            if (mConn.mSelfAddress.mType == FDB_SOCKET_IPC)
            {
                // For IPC socket, address of CTCPTransportSocket is the same as CLinuxServerSocket
                // So you can get address either from container or session
                conn_info.mSelfAddress = mConn.mSelfAddress;
            }
            else
            {
                bool secure = false;
                if (params.mSSLContext)
                {
                    if (!ret->createSSL(params.mSSLContext, false))
                    {
                        delete ret;
                        ret = 0;
                    }
                    else
                    {
                        secure = true;
                    }
                }
                if (ret)
                {
                    CBaseSocketFactory::buildUrl(conn_info.mSelfAddress.mUrl,
                                                 conn_info.mSelfAddress.mAddr.c_str(),
                                                 conn_info.mSelfAddress.mPort,
                                                 secure);
                }
            }
        }
    }
    catch (...)
    {
        ret = 0;
    }
    return ret;
}

CLinuxServerSocket::CLinuxServerSocket(CFdbSocketAddr &addr)
    : CServerSocketImp(addr)
    , mServerSocketImp(0)
{
}

CLinuxServerSocket::~CLinuxServerSocket()
{
    if (mServerSocketImp)
    {
        delete mServerSocketImp;
    }
}

bool CLinuxServerSocket::bind()
{
    bool ret = false;
    try
    {
        if (mServerSocketImp)
        {
        }
        else
        {
            if (mConn.mSelfAddress.mType == FDB_SOCKET_TCP)
            {
                sckt::IPAddress address(mConn.mSelfAddress.mAddr.c_str(), mConn.mSelfAddress.mPort);
                mServerSocketImp = new sckt::TCPServerSocket(address);
                mConn.mSelfAddress.mPort = mServerSocketImp->self_port; // in case port number is allocated dynamically...
            }
#ifndef __WIN32__
            else if (mConn.mSelfAddress.mType == FDB_SOCKET_IPC)
            {
                sckt::IPAddress address(mConn.mSelfAddress.mAddr.c_str());
                mServerSocketImp = new sckt::TCPServerSocket(address);
            }
#endif
            else
            {
                return 0;
            }
        }
        ret = true;
    }
    catch (...)
    {
        ret = false;
    }
    return ret;

}

CSocketImp *CLinuxServerSocket::accept(const CSocketParams &params)
{
    CSocketImp *ret = 0;
    sckt::TCPSocket *sock_imp = 0;
    sckt::Options opt(!params.mBlockMode, params.mKeepAliveInterval, params.mKeepAliveRetry);
    try
    {
        if (mServerSocketImp)
        {
            sock_imp = new sckt::TCPSocket();
            mServerSocketImp->Accept(*sock_imp, &opt);
            ret = new CTCPTransportSocket(sock_imp, mConn.mSelfAddress.mType);
            CFdbSocketConnInfo &conn_info = const_cast<CFdbSocketConnInfo &>(ret->getConnectionInfo());
            if (mConn.mSelfAddress.mType == FDB_SOCKET_IPC)
            {
                // For IPC socket, address of CTCPTransportSocket is the same as CLinuxServerSocket
                // So you can get address either from container or session
                conn_info.mSelfAddress = mConn.mSelfAddress;
            }
            else
            {
                bool secure = false;
                if (params.mSSLContext)
                {
                    if (!ret->createSSL(params.mSSLContext, true))
                    {
                        delete ret;
                        ret = 0;
                    }
                    else
                    {
                        secure = true;
                    }
                }
                if (ret)
                {
                    CBaseSocketFactory::buildUrl(conn_info.mSelfAddress.mUrl,
                                                 conn_info.mSelfAddress.mAddr.c_str(),
                                                 conn_info.mSelfAddress.mPort,
                                                 secure);
                }
            }
        }
    }
    catch (...)
    {
        if (sock_imp)
        {
            delete sock_imp;
        }
        ret = 0;
    }
    return ret;
}

int CLinuxServerSocket::getFd()
{
    if (mServerSocketImp)
    {
        return mServerSocketImp->getNativeSocket();
    }
    return -1;
}

bool getIpAddressLinux(CFdbInterfaceTable &addr_tbl)
{
    try
    {
        sckt::getIpAddress(addr_tbl);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

bool isValidIpAddrLinux(const char *addr)
{
    return sckt::isValidIpAddr(addr);
}

bool sameSubnetLinux(const char *ip1, const char *ip2, const char *mask)
{
    bool match = false;
    try
    {
        match = sckt::sameSubnet(ip1, ip2, mask);
    }
    catch (...)
    {
        return false;
    }

    return match;
}

CUDPTransportSocket::CUDPTransportSocket(sckt::UDPSocket *imp)
    : mSocketImp(imp)
    , mPeerUDPPort(FDB_INET_PORT_INVALID)
{
    mConn.mSelfAddress.mType = FDB_SOCKET_UDP;
}

CUDPTransportSocket::~CUDPTransportSocket()
{
    if (mSocketImp)
    {
        delete mSocketImp;
    }
}

bool CUDPTransportSocket::setUDPDestAddr(const char *ip_addr, int32_t port)
{
    if (!CBaseSocketFactory::parseIp(ip_addr, mPeerUDPIp))
    {
        return false;
    }
    mPeerUDPIpStr = ip_addr;
    mPeerUDPPort = port;
    return true;
}

int32_t CUDPTransportSocket::send(const uint8_t *data, int32_t size)
{
    if (!FDB_VALID_PORT(mPeerUDPPort))
    {
        return -1;
    }
    return send(data, size, mPeerUDPIp, mPeerUDPPort);
}

int32_t CUDPTransportSocket::send(const uint8_t *data, int32_t size, tFdbIpV4 ip, int32_t port)
{
    int32_t ret = -1;
    if (mSocketImp)
    {
        try
        {
            sckt::IPAddress address(ip, port);
            ret = mSocketImp->Send(data, size, address);
        }
        catch(...)
        {
            ret = -1;
        }
    }
    return ret;
}

int32_t CUDPTransportSocket::recv(uint8_t *data, int32_t size)
{
    tFdbIpV4 dummy_ip;
    int32_t dummy_port;
    return recv(data, size, dummy_ip, dummy_port);
}

int32_t CUDPTransportSocket::recv(uint8_t *data, int32_t size, tFdbIpV4 &ip, int32_t &port)
{
    int32_t ret = -1;
    if (mSocketImp)
    {
        try
        {
            sckt::IPAddress sender_ip;
            ret = mSocketImp->Recv(data, size, sender_ip);
            ip = sender_ip.host;
            port = sender_ip.port;
        }
        catch(...)
        {
            ret = -1;
        }
    }
    return ret;
}

int CUDPTransportSocket::getFd()
{
    if (mSocketImp)
    {
        return mSocketImp->getNativeSocket();
    }
    return -1;
}

CLinuxUDPSocket::CLinuxUDPSocket(CFdbSocketAddr &addr)
    : CUDPSocketImp(addr)
{
}

CSocketImp *CLinuxUDPSocket::bind()
{
    CSocketImp *ret = 0;
    try
    {
        sckt::UDPSocket *sckt_imp = 0;
        if (mConn.mSelfAddress.mType == FDB_SOCKET_UDP)
        {
            if (mConn.mSelfAddress.mAddr.empty())
            {
                mConn.mSelfAddress.mAddr = "127.0.0.1";
            }
            sckt::IPAddress address(mConn.mSelfAddress.mAddr.c_str(), mConn.mSelfAddress.mPort);
            sckt_imp = new sckt::UDPSocket();
            sckt_imp->Open(address);
        }

        if (sckt_imp)
        {
            ret = new CUDPTransportSocket(sckt_imp);
            CFdbSocketConnInfo &conn_info = const_cast<CFdbSocketConnInfo &>(ret->getConnectionInfo());
            // For UDP socket, address of CUDPTransportSocket is the same as CLinuxUDPSocket
            // So you can get address either from container or session
            conn_info.mSelfAddress = mConn.mSelfAddress;
            if (sckt_imp->self_port > 0)
            {   // port number might be allocated by system
                conn_info.mSelfAddress.mPort = sckt_imp->self_port;
            }
        }
    }
    catch (...)
    {
        ret = 0;
    }
    return ret;
}
}
}

