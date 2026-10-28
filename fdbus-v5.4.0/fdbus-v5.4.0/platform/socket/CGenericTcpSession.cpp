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

#include <fdbus/CGenericTcpSession.h>
#include <fdbus/CSocketImp.h>
#include <fdbus/CBaseWorker.h>
#include <utils/Log.h>

namespace ipc {
namespace fdbus {
#define FDB_SEND_RETRIES (1024 * 10)
#define FDB_SEND_DELAY 2
#define FDB_SEND_MAX_RECURSIVE 128

#define FDB_RECV_RETRIES FDB_SEND_RETRIES
#define FDB_RECV_DELAY FDB_SEND_DELAY

int32_t CGenericTcpSession::sendSync(const uint8_t *data, int32_t size)
{
    if (fatalError() || !data)
    {
        return -1;
    }

    int32_t bytes_sent = size;
    int32_t cnt = 0;
    int32_t retries = FDB_SEND_RETRIES;
    mRecursiveDepth++;
    while (1)
    {
        cnt = mSocketImp->send((uint8_t *)data, size);
        if (cnt < 0)
        {
            break;
        }
        data += cnt;
        size -= cnt;
        retries--;
        if ((size <= 0) || (retries <= 0))
        {
            break;
        }
        if (mRecursiveDepth < FDB_SEND_MAX_RECURSIVE)
        {
            worker()->dispatchInput(FDB_SEND_DELAY >> 1);
            sysdep_sleep(FDB_SEND_DELAY >> 1);
        }
        else
        {
            // Sorry just slow down...
            sysdep_sleep(FDB_SEND_DELAY);
        }
    }
    mRecursiveDepth--;

    if ((cnt < 0) || (size > 0))
    {
        if (cnt < 0)
        {
            LOG_E("CFdbSession: error or peer drops when writing %d bytes!\n", size);
        }
        else
        {
            LOG_E("CFdbSession: fail to write %d bytes!\n", size);
        }
        fatalError(true);
        return -1;
    }

    return bytes_sent;
}

int32_t CGenericTcpSession::sendSync(const uint8_t *data, int32_t size, tFdbIpV4 ip, int32_t port)
{
    return sendSync(data, size);
}
int32_t CGenericTcpSession::sendAsync(const uint8_t *data, int32_t size)
{
    submitOutput(data, size, 0, 0);
    return size;
}
int32_t CGenericTcpSession::sendAsync(const uint8_t *data, int32_t size, tFdbIpV4 ip, int32_t port)
{
    return sendAsync(data, size);
}

int32_t CGenericTcpSession::recvSync(uint8_t *data, int32_t size)
{
    int32_t byte_received = size;
    int32_t cnt = 0;
    int32_t retries = FDB_RECV_RETRIES;
    while (1)
    {
        cnt = mSocketImp->recv(data, size);
        if (cnt < 0)
        {
            break;
        }
        data += cnt;
        size -= cnt;
        retries--;
        if ((size <= 0) || (retries <= 0))
        {
            break;
        }
        sysdep_sleep(FDB_RECV_DELAY);
    }

    if ((cnt < 0) || (size > 0))
    {
        if (cnt < 0)
        {
            LOG_E("CFdbSession: error or peer drops when reading %d bytes!\n", size);
        }
        else
        {
            LOG_E("CFdbSession: fail to read %d bytes!\n", size);
        }
        fatalError(true);
        return -1;
    }

    return byte_received;
}

int32_t CGenericTcpSession::recvAsync(uint8_t *data, int32_t size)
{
    return mSocketImp->recv(data, size);
}

int32_t CGenericTcpSession::recvSync(uint8_t *data, int32_t size, tFdbIpV4 &ip, int32_t &port)
{
    return recvSync(data, size);
}

int32_t CGenericTcpSession::recvAsync(uint8_t *data, int32_t size, tFdbIpV4 &ip, int32_t &port)
{
    return recvAsync(data, size);
}

int32_t CGenericTcpSession::writeStream(const uint8_t *data, int32_t size)
{
    return mSocketImp->send((uint8_t *)data, size);
}
}
}

