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

#include <fdbus/CGenericUdpSession.h>
#include <fdbus/CSocketImp.h>

namespace ipc {
namespace fdbus {
int32_t CGenericUdpSession::sendSync(const uint8_t *data, int32_t size)
{
    return mSocketImp->send(data, size);
}

int32_t CGenericUdpSession::sendSync(const uint8_t *data, int32_t size, tFdbIpV4 ip, int32_t port)
{
    return mSocketImp->send(data, size, ip, port);
}
int32_t CGenericUdpSession::sendAsync(const uint8_t *data, int32_t size)
{
    return mSocketImp->send(data, size);
}
int32_t CGenericUdpSession::sendAsync(const uint8_t *data, int32_t size, tFdbIpV4 ip, int32_t port)
{
    return mSocketImp->send(data, size, ip, port);
}

int32_t CGenericUdpSession::recvSync(uint8_t *data, int32_t size)
{
    return mSocketImp->recv(data, size);
}

int32_t CGenericUdpSession::recvAsync(uint8_t *data, int32_t size)
{
    return mSocketImp->recv(data, size);
}

int32_t CGenericUdpSession::recvSync(uint8_t *data, int32_t size, tFdbIpV4 &ip, int32_t &port)
{
    return mSocketImp->recv(data, size, ip, port);
}

int32_t CGenericUdpSession::recvAsync(uint8_t *data, int32_t size, tFdbIpV4 &ip, int32_t &port)
{
    return mSocketImp->recv(data, size, ip, port);
}
}
}

