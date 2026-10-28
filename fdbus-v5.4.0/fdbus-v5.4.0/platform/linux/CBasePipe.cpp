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

#include <fcntl.h>
#include <unistd.h>
#include <fdbus/CBasePipe.h>

namespace ipc {
namespace fdbus {
CBasePipe::CBasePipe()
    : mReadFd(INVALID_FD)
    , mWriteFd(INVALID_FD)
{
}


CBasePipe::~CBasePipe()
{
    (void)close();
}

static void set_nonblock(int32_t fd)
{
    auto flags = fcntl(fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flags);
}

static void set_cloexec(int32_t fd)
{
    auto flags = fcntl(fd, F_GETFD, 0);
    flags |= FD_CLOEXEC;
    fcntl(fd, F_SETFD, flags);
}

bool CBasePipe::open
(
    bool  blockOnRead,
    bool  blockOnWrite
)
{
    int32_t fds[2] = {INVALID_FD, INVALID_FD};
    int32_t rc = pipe(fds);
    if ((0 == rc) && (fds[0] != INVALID_FD) && (fds[1] != INVALID_FD))
    {
        set_cloexec(fds[0]);
        set_cloexec(fds[1]);
        // If reads are non-blocking then ...
        if ( !blockOnRead )
        {
            set_nonblock(fds[0]);
        }

        // if writes are non-blocking then ...
        if ( !blockOnWrite )
        {
            set_nonblock(fds[1]);
        }

        mReadFd = fds[0];
        mWriteFd = fds[1];
        return true;
    }

    return false;
}


bool CBasePipe::close()
{
    bool closed(true);

    if ( INVALID_FD != mReadFd )
    {
        if ( 0 != ::close(mReadFd) )
        {
            closed = false;
        }
        mReadFd = INVALID_FD;
    }

    if ( INVALID_FD != mWriteFd )
    {
        if ( 0 != ::close(mWriteFd) )
        {
            closed = false;
        }
        mWriteFd = INVALID_FD;
    }

    return closed;
}


int32_t CBasePipe::getReadFd() const
{
    return mReadFd;
}


int32_t CBasePipe::getWriteFd() const
{
    return mWriteFd;
}


int32_t CBasePipe::read
(
    void    *buf,
    uint32_t nBytes
)
{
    return (int32_t)::read(mReadFd, buf, nBytes);
}


int32_t CBasePipe::write
(
    const void *buf,
    uint32_t nBytes
)
{
    return (int32_t)::write(mWriteFd, buf, nBytes);
}
}
}

