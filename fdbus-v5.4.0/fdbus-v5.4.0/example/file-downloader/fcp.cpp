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

#include <string>
#include <stdio.h>
#include "fcp-protocol.h"
using namespace std::placeholders;
using namespace ipc::fdbus;

static int32_t fcp_default_block_size = 1400;
static int32_t fcp_nr_download_progress = 80;
static int32_t fcp_nr_animation_progress = fcp_nr_download_progress << 3;

struct CTransferCtrl
{
    FILE *mFp;
    std::string mFile;
    int32_t mOffset;
    int32_t mCurrentDwnProgress;
    int32_t mCurrentAniProgress;
    int32_t mTotalSize;
};

static void start_download(CFdbBaseObject *obj, CTransferCtrl &ctrl, CBaseWorker *worker);

static void print_progress(CTransferCtrl &ctrl)
{
    auto unit = ctrl.mTotalSize / fcp_nr_animation_progress;
    if (!unit)
    {
        return;
    }
    auto next_progress = ctrl.mOffset / unit;
    if (next_progress <= ctrl.mCurrentAniProgress)
    {
        return;
    }
    ctrl.mCurrentAniProgress = next_progress;
    static const char *running_animation[] = {"|", "/", "-", "\\"};
    auto animation_mark = ctrl.mCurrentAniProgress % Fdb_Num_Elems(running_animation);

    unit = ctrl.mTotalSize / fcp_nr_download_progress;
    next_progress = unit ?  ctrl.mOffset / unit : 0; 
    ctrl.mCurrentDwnProgress = next_progress;

    auto nr_space_char = fcp_nr_download_progress - ctrl.mCurrentDwnProgress;
    std::string str_to_print = std::string(ctrl.mCurrentDwnProgress, '>') +
                               std::string(nr_space_char, ' ') +
                               running_animation[animation_mark];
    printf("%s\r", str_to_print.c_str());
    fflush(stdout);
}

static void on_file_block_ready(CBaseJob::Ptr &msg_ref, CFdbBaseObject *obj, CTransferCtrl ctrl)
{
    auto msg = castToMessage<CFdbMessage *>(msg_ref);
    if (msg->isStatus())
    {
        printf("\n");
        int32_t error_code;
        std::string reason;
        if (msg->decodeStatus(error_code, reason))
        {
            printf("Fail! status is received. Code: %d; reason: %s\n", error_code, reason.c_str());
        }
        fclose(ctrl.mFp);
        exit(0);
    }
    int32_t block_size = msg->getPayloadSize();
    auto size_remain = ctrl.mTotalSize - ctrl.mOffset;
    if (block_size > size_remain)
    {
        printf("\nFail! bad block size received: block size: %d; remain size: %d\n", block_size, size_remain);
        exit(-6);
    }

    fwrite(msg->getPayloadBuffer(), 1, msg->getPayloadSize(), ctrl.mFp);

    ctrl.mOffset += block_size;

    print_progress(ctrl);

    if (ctrl.mOffset >= ctrl.mTotalSize)
    {
        printf("\nSuccess! File %s is downloaded.\n", ctrl.mFile.c_str());
        fclose(ctrl.mFp);
        exit(0);
    }

    start_download(obj, ctrl, msg->worker());
}

static void start_download(CFdbBaseObject *obj, CTransferCtrl &ctrl, CBaseWorker *worker)
{
    CFdbBaseObject::tInvokeCallbackFn callback = std::bind(&on_file_block_ready, _1, _2, ctrl);
    FcpNextBlockReq req;
    req.mOffset = ctrl.mOffset;
    req.mSize = fcp_default_block_size;
    CFdbParcelableBuilder builder(req);
    obj->invoke(FCP_MSG_NEXT_BLOCK, builder, callback, worker);
}

int32_t main(int argc, char **argv)
{
    const char *source_file = 0;
    const char *to_file = 0;
    const char *public_key = 0;
    const char *private_key = 0;
    const char *root_ca = 0;
    int32_t help = 0;
    int32_t block_size = 0;
    const struct fdb_option core_options[] = {
        { FDB_OPTION_STRING, "source_file", 's', &source_file },
        { FDB_OPTION_STRING, "to_file", 't', &to_file },
        { FDB_OPTION_STRING, "public_key", 'p', &public_key },
        { FDB_OPTION_STRING, "private_key", 'v', &private_key },
        { FDB_OPTION_STRING, "root_ca", 'r', &root_ca },
        { FDB_OPTION_INTEGER, "block_size", 'b', &block_size },
        { FDB_OPTION_BOOLEAN, "help", 'h', &help }
    };
    fdb_parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);
    if (help)
    {
        printf("Usage: fcp -s source_file[ -t destination_file][-b block size][ -p public_key][ -v private_key][ -r root_ca]\n");
        return 0;
    }

    if (!source_file)
    {
        printf("Fail! -f source_file shall be specified!\n");
        return 0;
    }

    if (block_size)
    {
        if (block_size > FCP_MAX_BLOCK_SIZE)
        {
            block_size = FCP_MAX_BLOCK_SIZE;
        }
        fcp_default_block_size = block_size;
    }

    FILE *fp = 0;
    std::string std_to_file;
    if (!to_file)
    {
        std::string std_source_file(source_file);
        std::size_t found = std_source_file.find_last_of(FDB_PATH_SEPARATOR);
        if (found == std::string::npos)
        {
            std_to_file = source_file;
        }
        else
        {
            std_to_file = std_source_file.substr(found + 1);
        }
        fp = fopen(std_to_file.c_str(), "wb");
        if (!fp)
        {
            printf("Fail! Cannot open file to write: %s\n", std_to_file.c_str());
            return -3;
        }
    }

    FDB_CONTEXT->start();

    CBaseClient fcp_client("fcp_client");
    fcp_client.createSSL(public_key, private_key, root_ca);
    std::string fcp_url;
    ipc::fdbus::CBaseSocketFactory::buildUrl(fcp_url, FCP_SERVER_NAME);
    if (fcp_client.connect(fcp_url.c_str(), 5000) != ipc::fdbus::CONNECTED)
    {
        printf("Fail! Unable to connect with file server %s\n", fcp_url.c_str());
        return -1;
    }

    FcpStartSessionReq start_session_req;
    start_session_req.mFileName = source_file;
    CFdbParcelableBuilder builder(start_session_req);
    auto msg = new CFdbMessage(FCP_MSG_START_SESSION_REQ);
    CBaseJob::Ptr msg_ref(msg);
    fcp_client.invoke(msg_ref, builder);
    if (msg->isStatus())
    {
        int32_t error_code;
        std::string reason;
        if (msg->decodeStatus(error_code, reason))
        {
            printf("Fail! Status is received. Code: %d; reason: %s\n", error_code, reason.c_str());
        }
        return -2;
    }
    FcpStartSessionRep reply;
    CFdbParcelableParser parser(reply);
    if (!msg->deserialize(parser))
    {
        printf("Fail! Unable to parse reply to start session.\n");
        return -4;
    }

    CTransferCtrl ctrl = {fp,std_to_file, 0, 0, 0, reply.mTotalSize};

    start_download(&fcp_client, ctrl, CBaseWorker::create("fcp_worker"));

    /* convert main thread into worker */
    CBaseWorker background_worker;
    background_worker.start(FDB_WORKER_EXE_IN_PLACE);
}
