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

#include <fdbus/CGenericSession.h>
#include <fdbus/CGenericSocket.h>
#include <fdbus/CBaseSysDep.h>

namespace ipc {
namespace fdbus {
CGenericSession::CGenericSession(CGenericSocket *socket_handle, CSocketImp *sock_imp)
    : CBaseFdWatch(sock_imp->getFd(), POLLIN | POLLHUP | POLLERR)
    , mSocketHandle(socket_handle)
    , mSocketImp(sock_imp)
{}

CGenericSession::~CGenericSession()
{
    if (mSocketImp)
    {
        delete mSocketImp;
        mSocketImp = 0;
    }
}

void CGenericSession::onInput()
{
    if (fatalError())
    {
        return;
    }
    auto &session = mSocketHandle->findSession(this);
    if (session.get())
    {
        mSocketHandle->onPeerInput(session);
    }
}

void CGenericSession::onOutput()
{
    if (fatalError())
    {
        return;
    }
    auto &session = mSocketHandle->findSession(this);
    if (session.get())
    {
        mSocketHandle->onPeerOutput(session);
    }
}

void CGenericSession::onError()
{
    onHup();
}

void CGenericSession::onHup()
{
    auto session = mSocketHandle->findSession(this);
    if (session.get())
    {
        mSocketHandle->onPeerOffline(session);
        mSocketHandle->unregisterSession(this);
    }
}

int32_t CGenericSession::readStream(uint8_t *data, int32_t size)
{
    return mSocketImp->recv(data, size);
}
}
}

