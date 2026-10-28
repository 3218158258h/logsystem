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
 
#ifndef __CSVCADDRESSUTILS_H__
#define __CSVCADDRESSUTILS_H__

#include <list>
#include <fdbus/CSocketImp.h>
#include <fdbus/CFdbToken.h>
#include <fdbus/CBaseSocketFactory.h>

namespace ipc {
namespace fdbus {
class CFdbSession;
class FdbMsgExportableSvcAddress;
class FdbMsgAddressList;
class FdbMsgAddressItem;

template <class E>
class CFdbParcelableArray;

struct CFdbSocketAddrInfo
{
    CFdbSocketAddrInfo()
        : mUDPPort(FDB_INET_PORT_INVALID)
        , mExportableLevel(FDB_EXPORTABLE_DOMAIN)
        , mInterfaceId(FDB_INVALID_ID)
    {}
    CFdbSocketAddr mAddress;
    int32_t mUDPPort;
    int32_t mExportableLevel;
    int32_t mInterfaceId;
};

typedef std::list<CFdbSocketAddrInfo> tSocketAddrTbl;
struct CFdbSvcAddress
{
    tSocketAddrTbl mAddrTbl;
    FdbInstanceId_t mInstanceId;
    std::string mSvcName;
    std::string mEndpointName;
    std::string mHostName;
    std::string mDomainName;
    CFdbToken::tTokenList mTokens;
};
// mapping between service name and service address
typedef std::list<CFdbSvcAddress> tSvcAddrDescTbl;

class CSvcAddrUtils
{
public:
    static void populateFromHostInfo(const CFdbSvcAddress &exported_addr,
                                     FdbMsgAddressList &export_addr_list,
                                     int32_t exportable_level, bool remove = false);
    static void populateFromHostInfo(const tSvcAddrDescTbl &svc_addr_tbl,
                                     FdbMsgExportableSvcAddress &svc_address_tbl,
                                     int32_t exportable_level);
    static void populateToHostInfo(FdbMsgExportableSvcAddress &svc_address_tbl,
                                   tSvcAddrDescTbl &svc_addr_tbl);
    static void compareSvcTable(FdbMsgExportableSvcAddress &svc_address_tbl,
                                tSvcAddrDescTbl &mSvcAddrTbl,
                                tSvcAddrDescTbl &added_svc_tbl,
                                tSvcAddrDescTbl &removed_svc_tbl);
    static void printExternalSvc(const CFdbSvcAddress &svc, const char *prefix);
    static void printExternalSvcTbl(const tSvcAddrDescTbl &svc_tbl, const char *prefix);
    static void removeDuplicateService(tSvcAddrDescTbl &original, const tSvcAddrDescTbl &duplicate);
    static void validateUrl(FdbMsgExportableSvcAddress &msg_addr_tbl, CFdbSession *session);
    static void validateUrl(FdbMsgAddressList &msg_addr_list, CFdbSession *session);
    static void verifyUrl(CFdbParcelableArray<FdbMsgAddressItem> &addr_pool,
                                    CFdbSession *session);
    static void verifyUrl(FdbMsgExportableSvcAddress &msg_addr_tbl, CFdbSession *session);

private:
    static void populateToHostInfo(FdbMsgAddressList &addr_list, CFdbSvcAddress &exported_addr);
    static void setServiceInfo(const CFdbSvcAddress &exported_addr,
                               FdbMsgAddressList &export_addr_list);
    static bool getExportable(int32_t exportable, int32_t &level, bool &at_least);
    static bool checkExportable(int32_t current, int32_t desired, bool at_least);
    static void setAddressItem(const CFdbSocketAddrInfo &exported_item,
                               FdbMsgAddressItem *msg_item);
    static CFdbInterfaceTable mInterfaceTable;
};
}
}
#endif

