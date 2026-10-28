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

#include <utils/Log.h>
#include <string.h>
#include <fdbus/CSysFdWatch.h>
#include <fdbus/CFdEventLoop.h>
#include <fdbus/CFdbRawMsgBuilder.h>
#include <fdbus/CFdbBaseObject.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/CLogProducer.h>

namespace ipc {
namespace fdbus {
#define FDB_MAX_RECURSIVE_SIZE   	32

CSysFdWatch::CSysFdWatch(int fd, uint32_t flags)
    : mFd(fd)
    , mFlags(flags)
    , mEnable(false)
    , mFatalError(false)
    , mEventLoop(0)
    , mInputRecursiveDepth(0)
{}

CSysFdWatch::~CSysFdWatch()
{
    if (mFd > 0)
    {
        closePollFd(mFd);
    }

    clearOutputChunkList();
}

void CSysFdWatch::enable(bool enb)
{
    if (enb)
    {
        mFatalError = false;
    }
    mEventLoop->enableWatch(this, enb);
    mEnable = enb;
}

void CSysFdWatch::fatalError(bool enb)
{
    if (mFatalError != enb)
    {
        mEventLoop->rebuildPollFd();
    }
    mFatalError = enb;
}

void CSysFdWatch::updateFlags(uint32_t mask, uint32_t value)
{
    uint32_t flags = (mFlags & ~mask) | value;
    if (mFlags != flags)
    {
        mFlags = flags;
        mEventLoop->rebuildPollFd();
    }
}

CSysFdWatch::CInputDataChunk::CInputDataChunk()
    : mBuffer(0)
    , mSize(0)
    , mOffset(0)
    , mConsumed(0)
{}

CSysFdWatch::CInputDataChunk::~CInputDataChunk()
{
    if (mBuffer)
    {
        free(mBuffer);
    }
}

int32_t CSysFdWatch::CInputDataChunk::extractData(uint8_t *output, int32_t size)
{
    if (size > mConsumed)
    {
        return size - mConsumed;
    }
    memcpy(output, getBufferHead(), size);
    mOffset += size;
    mConsumed -= size;
    return 0;
}

bool CSysFdWatch::CInputDataChunk::checkBufferSize(int32_t size_expected)
{
    if (size_expected > mMaxInputChunkSize)
    {
        LOG_E("checkBufferSize: expected size %d too big!\n", size_expected);
        return false;
    }

    if (!mBuffer)
    {
        mSize = CSysFdWatch::mDefaultInputChunkSize;
        if (mSize < size_expected)
        {
            mSize = size_expected + CSysFdWatch::mDefaultInputChunkSize;
        }
        mBuffer = (uint8_t *)malloc(mSize);
        mOffset = 0;
        mConsumed = 0;
        return true;
    }

    if (mConsumed)
    {
        if (getTailSize() < size_expected)
        {
            memcpy(mBuffer, mBuffer + mOffset, mConsumed);
            mOffset = 0;
        }
    }
    else
    {
        mOffset = 0;
    }

    if (mSize < size_expected)
    {
        mSize = size_expected + CSysFdWatch::mDefaultInputChunkSize;
        mBuffer = (uint8_t*)realloc(mBuffer, mSize);
    }
    return true;
}

void CSysFdWatch::CInputDataChunk::enable(bool enb)
{
    if (enb)
    {
        checkBufferSize(0);
    }
    else
    {
        if (mBuffer)
        {
            free(mBuffer);
            mBuffer = 0;
            mSize = 0;
            mOffset = 0;
            mConsumed = 0;
        }
    }
}

CSysFdWatch::COutputDataChunk::COutputDataChunk(const uint8_t *msg_buffer, int32_t msg_size,
                                                int32_t consumed,
                                                uint8_t *log_buffer, int32_t log_size)
    : mBuffer(0)
    , mSize(msg_size)
    , mConsumed(consumed)
    , mLogBuffer(log_buffer)
    , mLogSize(log_size)
{
    if (msg_size && msg_buffer)
    {
        try
        {
            mBuffer = new uint8_t[msg_size];
            memcpy(mBuffer, msg_buffer, msg_size);
        }
        catch (...)
        {
            LOG_E("COutputDataChunk: fail to allocate message with size %d\n", msg_size);
            mSize = 0;
            mConsumed = 0;
        }
    }
}

CSysFdWatch::COutputDataChunk::~COutputDataChunk()
{
    if (mLogBuffer)
    {
        delete[] mLogBuffer;
    }

    if (mBuffer)
    {
        delete[] mBuffer;
    }
}

void CSysFdWatch::submitOutput(const uint8_t *msg_buffer, int32_t msg_size,
                               uint8_t *log_buffer, int32_t log_size)
{
    if (!msg_buffer || !msg_size || fatalError())
    {
        if (log_buffer)
        {
            delete[] log_buffer;
        }
        return;
    }
    if (mOutputChunkList.size())
    {
        mOutputChunkList.push_back(new COutputDataChunk(msg_buffer, msg_size, 0, log_buffer, log_size));
    }
    else
    {
        auto consumed = writeStream(msg_buffer, msg_size);
        if (consumed < 0)
        {
            fatalError(true);
        }
        else if (consumed < msg_size)
        {
            mOutputChunkList.push_back(new COutputDataChunk(msg_buffer, msg_size, consumed, log_buffer, log_size));
            updateFlags(POLLOUT, POLLOUT);
        }
        else if (log_buffer)
        {
            auto logger = FDB_CONTEXT->getLogger();
            if (logger)
            {
                logger->sendLog(REQ_FDBUS_LOG, log_buffer, log_size);
            }
            delete[] log_buffer;
        }
    }
}

void CSysFdWatch::processInput()
{
    if (!mInputChunk.mBuffer)
    {
        onInput();
        return;
    }

    int32_t size_expected = 0;
    auto num_read = readStream(mInputChunk.getBufferTail(), mInputChunk.getTailSize());
    if (num_read < 0)
    {
        fatalError(true);
        size_expected = onInputReady();
    }
    else if (num_read > 0)
    {
        mInputChunk.mConsumed += num_read;
        size_expected = onInputReady();
    }

    if (!mInputChunk.checkBufferSize(size_expected))
    {
        fatalError(true);
        onInputReady();
    }
}

void CSysFdWatch::clearOutputChunkList()
{
    while (!mOutputChunkList.empty())
    {
        auto it = mOutputChunkList.begin();
        delete *it;
        mOutputChunkList.pop_front();
    }
}

void CSysFdWatch::processOutput()
{
    if (mOutputChunkList.empty())
    {
        onOutput();
        return;
    }

    while (!mOutputChunkList.empty())
    {
        auto it = mOutputChunkList.begin();
        auto chunk = *it;
        if (!chunk->mSize)
        {
            delete chunk;
            mOutputChunkList.pop_front();
            continue;
        }
        auto buffer = chunk->mBuffer + chunk->mConsumed;
        auto size = chunk->mSize - chunk->mConsumed;
        auto consumed = writeStream(buffer, size);
        if (consumed < 0)
        {
            fatalError(true);
            clearOutputChunkList();
            break;
        }
        else if (consumed < size)
        {
            chunk->mConsumed += consumed;
            break;
        }
        else
        {
            if (chunk->mLogBuffer)
            {
                auto logger = FDB_CONTEXT->getLogger();
                if (logger)
                {
                    logger->sendLog(REQ_FDBUS_LOG, chunk->mLogBuffer, chunk->mLogSize);
                }
            }
            delete chunk;
            mOutputChunkList.pop_front();
        }
    }

    if (mOutputChunkList.empty())
    {
        updateFlags(POLLOUT, 0);
    }
}
}
}
