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

#include <stdlib.h>
#include <string.h>
#include "linux/CLinuxSocket.h"
#include <fdbus/CBaseSocketFactory.h>
#include <fdbus/common_defs.h>
#include <utils/CNsConfig.h>
#ifdef CONFIG_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace ipc {
namespace fdbus {
CClientSocketImp *CBaseSocketFactory::createClientSocket(CFdbSocketAddr &addr)
{
    if ((addr.mType == FDB_SOCKET_TCP) || (addr.mType == FDB_SOCKET_IPC))
    {
        return new CLinuxClientSocket(addr);
    }

    return 0;
}

CClientSocketImp *CBaseSocketFactory::createClientSocket(const char *url)
{
    CFdbSocketAddr addr;
    if (!parseUrl(url, addr))
    {
        return 0;
    }

    return createClientSocket(addr);
}

CServerSocketImp *CBaseSocketFactory::createServerSocket(CFdbSocketAddr &addr)
{
    if ((addr.mType == FDB_SOCKET_TCP) || (addr.mType == FDB_SOCKET_IPC))
    {
        return new CLinuxServerSocket(addr);
    }

    return 0;
}

CServerSocketImp *CBaseSocketFactory::createServerSocket(const char *url)
{
    CFdbSocketAddr addr;
    if (!parseUrl(url, addr))
    {
        return 0;
    }

    return createServerSocket(addr);
}

CUDPSocketImp *CBaseSocketFactory::createUDPSocket(CFdbSocketAddr &addr)
{
    if (addr.mType == FDB_SOCKET_UDP)
    {
        return new CLinuxUDPSocket(addr);
    }

    return 0;
}

CUDPSocketImp *CBaseSocketFactory::createUDPSocket(const char *url)
{
    CFdbSocketAddr addr;
    if (!parseUrl(url, addr))
    {
        return 0;
    }

    return createUDPSocket(addr);
}

bool CBaseSocketFactory::parseUrl(const char *url, CFdbSocketAddr &addr)
{
    if (!url)
    {
        return false;
    }
    std::string u (url);
    std::string::size_type pos = u.find ("://");
    if (pos == std::string::npos)
    {
        return false;
    }
    std::string protocol = u.substr (0, pos);
    std::string addr_str = u.substr (pos + 3);

    if (protocol.empty () || addr_str.empty ())
    {
        return false;
    }

    addr.mUrl = url;
    addr.mSecure = false;
    if (protocol == FDB_URL_TCP_IND)
    {
        addr.mType = FDB_SOCKET_TCP;
        if (buildINetAddress(addr_str.c_str(), addr))
        {
            return false;
        }
    }
    else if (protocol == FDB_URL_UDP_IND)
    {
        addr.mType = FDB_SOCKET_UDP;
        if (buildINetAddress(addr_str.c_str(), addr))
        {
            return false;
        }
    }
    else if (protocol == FDB_URL_IPC_IND)
    {
        addr.mType = FDB_SOCKET_IPC;
        addr.mSecure = true;
        if (buildIPCAddress(addr_str.c_str(), addr))
        {
            return false;
        }
    }
    else if (protocol == FDB_URL_SVC_IND)
    {
        addr.mType = FDB_SOCKET_SVC;
        if (buildSvcAddress(addr_str.c_str(), addr))
        {
            return false;
        }
    }
    else if (protocol == FDB_URL_TCPS_IND)
    {
        addr.mType = FDB_SOCKET_TCP;
        addr.mSecure = true;
        if (buildINetAddress(addr_str.c_str(), addr))
        {
            return false;
        }
    }
    else if (protocol == FDB_URL_UDPS_IND)
    {
        addr.mType = FDB_SOCKET_UDP;
        addr.mSecure = true;
        if (buildINetAddress(addr_str.c_str(), addr))
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    return true;
}

int32_t CBaseSocketFactory::buildINetAddress(const char *host_addr, CFdbSocketAddr &addr)
{
    const char *delimiter = strrchr (host_addr, ':');
    if (!delimiter)
    {
        return -1;
    }
    std::string addr_str(host_addr, delimiter - host_addr);
    std::string port_str(delimiter + 1);

    //  Remove square brackets around the address, if any, as used in IPv6
    if (addr_str.size () >= 2 && addr_str [0] == '[' &&
            addr_str [addr_str.size () - 1] == ']')
    {
        addr_str = addr_str.substr (1, addr_str.size () - 2);
    }

    //  Allow 0 specifically, to detect invalid port error in atoi if not
    uint16_t port;
    if (port_str == "*" || port_str == "0")
        //  Resolve wildcard to 0 to allow autoselection of port
    {
        port = 0;
    }
    else
    {
        //  Parse the port number (0 is not a valid port).
        port = (uint16_t) atoi (port_str.c_str ());
        if (port == 0)
        {
            return -1;
        }
    }
    addr.mAddr = addr_str;
    addr.mPort = port;
    return 0;
}

int32_t CBaseSocketFactory::buildIPCAddress(const char *addr_str, CFdbSocketAddr &addr)
{
    addr.mAddr = addr_str;
    addr.mPort = 0;
    return 0;
}

int32_t CBaseSocketFactory::buildSvcAddress(const char *host_name, CFdbSocketAddr &addr)
{
    const char *delimiter = strrchr(host_name, ':');
    if (delimiter)
    {
        std::string svc_name(host_name, delimiter - host_name);
        std::string inst_id(delimiter + 1);
        FdbInstanceId_t id = FDB_DEFAULT_INSTANCE;
        if (!inst_id.empty())
        {
            if (inst_id.back() == FDB_ANY_INSTANCE_CHAR)
            {
                inst_id.pop_back();
                uint32_t group_id = FDB_EVENT_ALL_GROUPS;
                if (!inst_id.empty())
                {
                    try
                    {
                        group_id = std::stoi(inst_id, 0, 0);
                    }
                    catch (...)
                    {
                        return -1;
                    }
                }
                id = fdbMakeGroupEvent(group_id);
            }
            else
            {
                try
                {
                    id = std::stoi(inst_id, 0, 0);
                }
                catch (...)
                {
                    return -2;
                }
            }
        }
        addr.mAddr = svc_name;
        addr.mPort = id;
    }
    else
    {
        addr.mAddr = host_name;
        addr.mPort = FDB_DEFAULT_INSTANCE;
    }

    return 0;
}

bool CBaseSocketFactory::getIpAddress(CFdbInterfaceTable &addr_tbl)
{
    return getIpAddressLinux(addr_tbl);
}
bool CBaseSocketFactory::sameSubnet(const char *ip1, const char *ip2, const char *mask)
{
    return sameSubnetLinux(ip1, ip2, mask);
}

void CBaseSocketFactory::buildUrl(std::string &url, const char *ip_addr, const char *port, bool secure)
{
    url = secure ? FDB_URL_TCPS : FDB_URL_TCP;
    if (ip_addr)
    {
        url = url + ip_addr + ":" + port;
    }
    else
    {
        url = url + ":" + port;
    }
}

void CBaseSocketFactory::buildUrl(std::string &url, const char *ip_addr, int32_t port, bool secure)
{
    char port_string[64];
    sprintf(port_string, "%u", port);
    buildUrl(url, ip_addr, port_string, secure);
}

void CBaseSocketFactory::buildUrl(std::string &url, uint32_t ip_addr, int32_t port, bool secure,
                                  EFdbSocketType type)
{
    if (type == FDB_SOCKET_TCP)
    {
        url = secure ? FDB_URL_TCPS : FDB_URL_TCP;
    }
    else if (type == FDB_SOCKET_UDP)
    {
        url = secure ? FDB_URL_UDPS : FDB_URL_UDP;
    }
    url += std::to_string((uint8_t)((ip_addr >> 0) & 0xff));
    url += ".";
    url += std::to_string((uint8_t)((ip_addr >> 8) & 0xff));
    url += ".";
    url += std::to_string((uint8_t)((ip_addr >> 16) & 0xff));
    url += ".";
    url += std::to_string((uint8_t)((ip_addr >> 24) & 0xff));
    url += ":";
    url += std::to_string((uint16_t)port);
}

void CBaseSocketFactory::buildUrl(std::string &url, uint32_t uds_id, const char *ipc_path)
{
    if (ipc_path)
    {
        url = FDB_URL_IPC;
        url += ipc_path;
    }
    else
    {
        char uds_id_string[64];
        sprintf(uds_id_string, "%u", uds_id);
        
        url = CNsConfig::getIPCUrlBase();
        url += uds_id_string;
    }
}

void CBaseSocketFactory::buildUrl(std::string &url, const char *svc_name, FdbInstanceId_t instance_id)
{
    url = FDB_URL_SVC;
    url = url + svc_name;
    if (instance_id == FDB_DEFAULT_INSTANCE)
    {
        return;
    }
    if (fdbIsGroup(instance_id))
    {
        url = url + ":" + std::to_string((uint32_t)fdbEventGroup(instance_id));
        url = url + FDB_ANY_INSTANCE;
    }
    else
    {
        url = url + ":" + std::to_string((uint32_t)instance_id);
    }
}

void CFdbSocketAddr::buildUrl()
{
    if (mType == FDB_SOCKET_IPC)
    {
        mUrl = FDB_URL_IPC;
        mUrl = mUrl + ":" + mAddr;
    }
    else
    {
        CBaseSocketFactory::buildUrl(mUrl, mAddr.c_str(), mPort, mSecure);
    }
}

#ifdef CONFIG_SSL
void *CBaseSocketFactory::createSSL(const char *public_key,
                                    const char *private_key,
                                    const char *root_ca,
                                    uint32_t verify_type,
                                    bool is_client)
{
    if (!public_key && !private_key && !root_ca)
    {
        return 0;
    }

    SSL_CTX *ctx;
    if (is_client)
    {
        ctx = SSL_CTX_new(SSLv23_client_method());
    }
    else
    {
        ctx = SSL_CTX_new(SSLv23_server_method());
    }

    if (!ctx)
    {
        ERR_print_errors_fp(stdout);
        return 0;
    }
    if (root_ca)
    {
        SSL_CTX_set_verify(ctx, verify_type, 0);
        if (SSL_CTX_load_verify_locations(ctx, root_ca, 0) <= 0)
        {
            ERR_print_errors_fp(stdout);
            SSL_CTX_free(ctx);
            return 0;
        }
    }

    if (public_key)
    {
        if (SSL_CTX_use_certificate_file(ctx, public_key, SSL_FILETYPE_PEM) <= 0)
        {
            ERR_print_errors_fp(stdout);
            SSL_CTX_free(ctx);
            return 0;
        }
    }

    if (private_key)
    {
        if (SSL_CTX_use_PrivateKey_file(ctx, private_key, SSL_FILETYPE_PEM) <= 0)
        {
            ERR_print_errors_fp(stdout);
            SSL_CTX_free(ctx);
            return 0;
        }
        if (!SSL_CTX_check_private_key(ctx))
        {
            ERR_print_errors_fp(stdout);
            SSL_CTX_free(ctx);
            return 0;
        }
    }
    return (void *)ctx;
}
#else
void *CBaseSocketFactory::createSSL(const char *public_key,
                                    const char *private_key,
                                    const char *root_ca,
                                    uint32_t verify_type,
                                    bool is_client)
{
    return 0;
}
#endif

bool CBaseSocketFactory::parseIp(const char* ip, tFdbIpV4 &int_ip){
    if(!ip || (ip[0] == '\0'))
    {
        int_ip = 0;
        return true;
    }
    if ((ip[0] == '0') && (ip[1] == '\0'))
    {
        int_ip = 0;
        return true;
    }

    tFdbIpV4 h = 0;//parsed host
    const char *curp = ip;
    for(uint32_t t=0; t<4; ++t){
        uint32_t dights[3];
        uint32_t numDgts;
        for(numDgts=0; numDgts<3; ++numDgts){
            if( *curp == '.' || *curp == 0 ){
                if(numDgts==0)
                    return false;
                break;
            }else{
                if(*curp < '0' || *curp > '9')
                    return false;
                dights[numDgts] = uint32_t(*curp)-uint32_t('0');
            }
            ++curp;
        }

        if(t<3 && *curp != '.')//unexpected delimiter or unexpected end of string
            return false;
        else if(t==3 && *curp != 0)
            return false;

        uint32_t xxx=0;
        for(uint32_t i=0; i<numDgts; ++i){
            uint32_t ord = 1;
            for(uint32_t j=1; j<numDgts-i; ++j)
               ord*=10;
            xxx+=dights[i]*ord;
        }
        if(xxx > 255)
            return false;

        h |= (xxx<<(8*t));

        ++curp;
    }
    int_ip = h;
    return true;
};

bool CBaseSocketFactory::isValidIpAddr(const char *addr)
{
    return isValidIpAddrLinux(addr);
}
}
}

