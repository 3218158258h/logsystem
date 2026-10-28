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
#ifndef __CADDRESSALLOCATOR_H__
#define __CADDRESSALLOCATOR_H__

#include <string>
#include <fdbus/CSocketImp.h>

namespace ipc {
namespace fdbus {
enum FdbServerType
{
    FDB_SVC_NAME_SERVER,
    FDB_SVC_HOST_SERVER,
    FDB_SVC_LOG_SERVER,
    FDB_SVC_USER,
    FDB_SVC_UNKNOWN
};

struct CAllocatedAddress
{
    CFdbSocketAddr mSocketAddr;
    int32_t mExportableLevel;
    int32_t mInterfaceId;
};

class IAddressAllocator
{
public:
    virtual void allocate(CAllocatedAddress &alloc_addr, FdbServerType svc_type, bool secure) = 0;
    virtual bool allocateKnownAddr(CAllocatedAddress &alloc_addr, FdbServerType svc_type, bool secure) = 0;
    virtual void reset() = 0;
    virtual ~IAddressAllocator() {}
    virtual int32_t exportableLevel() const
    {
        return FDB_EXPORTABLE_NODE_INTERNAL;
    }
    virtual void setExportableLevel(int32_t level)
    {}
    virtual int32_t interfaceId() const
    {
        return FDB_INVALID_ID;
    }
    virtual void setInterfaceId(int32_t id)
    {}
    static FdbServerType getSvcType(const char *svc_name);
};

class CIPCAddressAllocator : public IAddressAllocator
{
public:
    CIPCAddressAllocator();
    void allocate(CAllocatedAddress &alloc_addr, FdbServerType svc_type, bool secure);
    bool allocateKnownAddr(CAllocatedAddress &alloc_addr, FdbServerType svc_type, bool secure);
    void reset();

private:
    uint32_t mSocketId;
};

class CTCPAddressAllocator : public IAddressAllocator
{
public:
    CTCPAddressAllocator();
    void allocate(CAllocatedAddress &alloc_addr, FdbServerType svc_type, bool secure);
    bool allocateKnownAddr(CAllocatedAddress &alloc_addr, FdbServerType svc_type, bool secure);
    void reset();
    const std::string interfaceIp() const
    {
        return mInterfaceIp;
    }
    void setInterfaceIp(const char *ip_addr)
    {
        mInterfaceIp = ip_addr;
    }
    void setExportableLevel(int32_t level)
    {
        mExportableLevel = level;
    }
    int32_t exportableLevel() const
    {
        return mExportableLevel;
    }
    virtual int32_t interfaceId() const
    {
        return mInterfaceId;
    }
    virtual void setInterfaceId(int32_t id)
    {
        mInterfaceId = id;
    }
    void setMask(const char *mask)
    {
        mMask = mask;
    }
    const std::string &mask() const
    {
        return mMask;
    }

private:
    int32_t mMinPort;
    int32_t mMaxPort;
    int32_t mPort;
    std::string mInterfaceIp;
    std::string mMask;
    int32_t mExportableLevel;
    int32_t mInterfaceId;
    bool mExportable;
};

class CUDPPortAllocator
{
public:
    CUDPPortAllocator();
    int32_t allocate();
    void reset();
private:
    int32_t mMinPort;
    int32_t mMaxPort;
    int32_t mPort;
};
}
}
#endif
