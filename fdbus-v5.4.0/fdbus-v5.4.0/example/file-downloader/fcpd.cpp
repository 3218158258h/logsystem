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

#include <map>
#include "fcp-protocol.h"
#ifdef __WIN32__
#include <direct.h>
#else
#include <unistd.h>
#endif

using namespace std::placeholders;
using namespace ipc::fdbus;

struct CTransferCtrl
{
    std::string mFile;
    FILE *mFp;
    int32_t mTotalSize;
    int32_t mOffset;

    CTransferCtrl(const char *file_name, FILE *fp, int32_t size)
        : mFile(file_name)
        , mFp(fp)
        , mTotalSize(size)
        , mOffset(0)
    {
    }
    ~CTransferCtrl()
    {
        if (mFp)
        {
            fclose(mFp);
        }
    }
};

typedef std::map<FdbSessionId_t, CTransferCtrl> tTransferCtrl;
static tTransferCtrl sTransferCtrl;

static void on_start_session(CBaseJob::Ptr &msg_ref, CFdbBaseObject *obj)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    const char *sender_name = msg->senderName().c_str();

    auto it = sTransferCtrl.find(msg->session());
    if (it != sTransferCtrl.end())
    {
        msg->status(msg_ref, FCP_ERR_BUSY, "A download session is on-going!");
        return;
    }

    FcpStartSessionReq req;
    CFdbParcelableParser parser(req);
    if (!msg->deserialize(parser))
    {
        printf("%s: unable to parse request!\n", sender_name);
        msg->status(msg_ref, FCP_ERR_DATA, "bad data format");
        return;
    }

    FILE *fp = fopen(req.mFileName.c_str(), "rb");
    if (!fp)
    {
        printf("%s: unable to open %s!\n", sender_name, req.mFileName.c_str());
        msg->statusf(msg_ref, FCP_ERR_FILE_ACCESS, "unable to open %s", req.mFileName.c_str());
        return;
    }

    fseek (fp, 0, SEEK_END);
    auto file_size = (int32_t)ftell(fp);
    rewind(fp);

    sTransferCtrl.emplace(std::piecewise_construct,
                          std::forward_as_tuple(msg->session()),
                          std::forward_as_tuple(req.mFileName.c_str(), fp, file_size));

    FcpStartSessionRep rep;
    rep.mErrorCode = FCP_ERR_OK;
    rep.mTotalSize = file_size;
    CFdbParcelableBuilder builder(rep);
    msg->reply(msg_ref, builder);
}

static void on_next_block(CBaseJob::Ptr &msg_ref, CFdbBaseObject *obj)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    const char *sender_name = msg->senderName().c_str();

    auto it = sTransferCtrl.find(msg->session());
    if (it == sTransferCtrl.end())
    {
        msg->status(msg_ref, FCP_ERR_IDLE, "No download session is on-going");
        return;
    }

    FcpNextBlockReq req;
    CFdbParcelableParser parser(req);
    if (!msg->deserialize(parser))
    {
        printf("%s: unable to parse request!\n", sender_name);
        msg->status(msg_ref, FCP_ERR_DATA, "bad data format");
        sTransferCtrl.erase(it);
        return;
    }

    auto &ctrl = it->second;
    if (req.mOffset != ctrl.mOffset)
    {
        printf("%s: Download progress doesn't match! Client: %d, Server: %d\n",
                sender_name, req.mOffset, ctrl.mOffset);
        msg->status(msg_ref, FCP_ERR_TRANSFER, "transfer progress doesn't match");
        sTransferCtrl.erase(it);
        return;
    }

    int32_t transfer_size = req.mSize;
    int32_t remain_size = ctrl.mTotalSize - ctrl.mOffset;
    if (transfer_size > remain_size)
    {
        transfer_size = remain_size;
    }
    if (transfer_size > FCP_MAX_BLOCK_SIZE)
    {
        transfer_size = FCP_MAX_BLOCK_SIZE;
    }

    uint8_t buffer[FCP_MAX_BLOCK_SIZE];
    fread(buffer, transfer_size, 1, ctrl.mFp);

    ctrl.mOffset += transfer_size;
    if (ctrl.mOffset >= ctrl.mTotalSize)
    {
        printf("%s: Transfer of file %s done!\n", sender_name, ctrl.mFile.c_str());
        sTransferCtrl.erase(it);
    }

    msg->reply(msg_ref, buffer, transfer_size);
}

static void on_stop_session(CBaseJob::Ptr &msg_ref, CFdbBaseObject *obj)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    auto it = sTransferCtrl.find(msg->session());
    if (it == sTransferCtrl.end())
    {
        msg->status(msg_ref, FCP_ERR_IDLE, "No download session is on-going");
        return;
    }

    sTransferCtrl.erase(it);
}

static void on_client_connected(CFdbBaseObject *obj, const CFdbOnlineInfo &info, bool is_online)
{
    if (!is_online)
    {
        auto it = sTransferCtrl.find(info.mSid);
        if (it != sTransferCtrl.end())
        {
            sTransferCtrl.erase(it);
        }
    }
}

int32_t main(int argc, char **argv)
{
    const char *work_space = 0;
    const char *public_key = 0;
    const char *private_key = 0;
    const char *root_ca = 0;
    int32_t help = 0;
    const struct fdb_option core_options[] = {
        { FDB_OPTION_STRING, "work_space", 'w', &work_space },
        { FDB_OPTION_STRING, "public_key", 'p', &public_key },
        { FDB_OPTION_STRING, "private_key", 'v', &private_key },
        { FDB_OPTION_STRING, "root_ca", 'r', &root_ca },
        { FDB_OPTION_BOOLEAN, "help", 'h', &help }
    };
    fdb_parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);
    if (help)
    {
        printf("Usage: fcpd[ -w work_space][ -p public_key][ -v private_key][ -r root_ca]\n");
        return 0;
    }

    FDB_CONTEXT->start();

    CBaseServer fcp_server("file server");
    fcp_server.createSSL(public_key, private_key, root_ca);
    if (public_key || private_key || root_ca)
    {
        fcp_server.enableTCP(false);
    }
    
    if (work_space)
    {
        bool dir_changed;
#ifdef __WIN32__
        dir_changed = _chdir(work_space) == 0;
#else
        dir_changed = chdir(work_space) == 0;
#endif
        if (!dir_changed)
        {
            printf("Unable to enter work space: %s\n", work_space);
            return -1;
        }
    }

    CBaseWorker *worker = CBaseWorker::create("file server");
    CFdbBaseObject::tConnCallbackFn conn_fn = std::bind(&on_client_connected, _1, _2, _3);
    fcp_server.registerConnNotification(conn_fn, worker);

    CMsgHandleTbl msg_tbl;

    msg_tbl.add(FCP_MSG_START_SESSION_REQ, std::bind(&on_start_session, _1, _2), worker);
    msg_tbl.add(FCP_MSG_STOP_SESSION_REQ, std::bind(&on_stop_session, _1, _2), worker);
    msg_tbl.add(FCP_MSG_NEXT_BLOCK, std::bind(&on_next_block, _1, _2), worker);
    fcp_server.registerMsgHandle(msg_tbl);

    std::string fcp_url;
    ipc::fdbus::CBaseSocketFactory::buildUrl(fcp_url, FCP_SERVER_NAME);
    fcp_server.bind(fcp_url.c_str());

    /* convert main thread into worker */
    CBaseWorker background_worker;
    background_worker.start(FDB_WORKER_EXE_IN_PLACE);
}
