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

#include <fdbus/CGenericSocket.h>
#include <fdbus/CGenericSession.h>
#include <fdbus/CBaseSocketFactory.h>
#ifdef CONFIG_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace ipc {
namespace fdbus {
FdbScktSessionPtr CGenericSocket::mEmptyPtr;

#ifdef CONFIG_SSL
bool CGenericSocket::createSSL(const char *public_key,
                               const char *private_key,
                               const char *root_ca,
                               uint32_t verify_type)
{
    void *ssl_context = CBaseSocketFactory::createSSL(public_key,
                                                      private_key,
                                                      root_ca,
                                                      verify_type,
                                                      false);
    if (!ssl_context)
    {
        return false;
    }
    if (mScktParams.mSSLContext)
    {
        SSL_CTX_free((SSL_CTX *)mScktParams.mSSLContext);
    }
    mScktParams.mSSLContext = ssl_context;
    return true;
}
#else
bool CGenericSocket::createSSL(const char *public_key,
                               const char *private_key,
                               const char *root_ca,
                               uint32_t verify_type)
{
    return false;
}
#endif

void CGenericSocket::config(bool block_mode, int32_t ka_interval, int32_t ka_retry)
{
    mScktParams.mBlockMode = block_mode;
    mScktParams.mKeepAliveInterval = ka_interval;
    mScktParams.mKeepAliveRetry = ka_retry;
}
}
}

