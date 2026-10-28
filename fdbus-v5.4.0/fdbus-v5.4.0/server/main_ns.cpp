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

#include <stdlib.h>
#include <fdbus/CFdbContext.h>
#include <fdbus/fdb_option_parser.h>
#include "CNameServer.h"
#include <iostream>

using namespace ipc::fdbus;

int main(int argc, char **argv)
{
    char *url_host_servers = 0;
    char *host_name = 0;
    char *interfaces = 0;
    char *url_static_hosts = 0;
    int32_t help = 0;
    int32_t domain_exportable_level = -1;
    int32_t upstream_exportable_level = -1;
    int32_t self_exportable_level = -1;
    char *watchdog_params = 0;
    int32_t force_bind_address = 0;
    const struct fdb_option core_options[] = {
        { FDB_OPTION_STRING, "url", 'u', &url_host_servers },
        { FDB_OPTION_STRING, "name", 'n', &host_name },
        { FDB_OPTION_STRING, "interface ip list", 'i', &interfaces },
        { FDB_OPTION_STRING, "static host url", 's', &url_static_hosts },
        { FDB_OPTION_STRING, "watchdog", 'd', &watchdog_params },
        { FDB_OPTION_INTEGER, "domain_level", 'o', &domain_exportable_level },
        { FDB_OPTION_INTEGER, "upstream_level", 'p', &upstream_exportable_level },
        { FDB_OPTION_INTEGER, "self_level", 'l', &self_exportable_level },
        { FDB_OPTION_BOOLEAN, "force_bind_address", 'f', &force_bind_address },
        { FDB_OPTION_BOOLEAN, "help", 'h', &help }
    };

    fdb_parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);
    if (help)
    {
        std::cout << "FDBus - Fast Distributed Bus" << std::endl;
        std::cout << "    SDK version " << FDB_DEF_TO_STR(FDB_VERSION_MAJOR) "."
                                           FDB_DEF_TO_STR(FDB_VERSION_MINOR) "."
                                           FDB_DEF_TO_STR(FDB_VERSION_BUILD) << std::endl;
        std::cout << "    LIB version " << CFdbContext::getFdbLibVersion() << std::endl;
        std::cout << "Usage: name_server[ -n host_name][ -u host_url1,host_url2...][ -i ip1,ip2...][-l exportable level][-p upstream exportable level][-o domain exportable level]" << std::endl;
        std::cout << "Service naming server" << std::endl;
        std::cout << "    -n host_name: host name of this machine" << std::endl;
        std::cout << "    -u host_url1,host_url2...: the URL of host servers to be connected" << std::endl;
        std::cout << "    -i ip1+level+id,ip2+level+id...: interfaces to listen on for services. " << std::endl;
        std::cout << "    -l self_level: specify exportable level of name server" << std::endl;
        std::cout << "    -o domain_level: specify the maximum level that should be exported within the domain" << std::endl;
        std::cout << "    -p upstream_level: specify the minimum level that should be exported to upstream host server" << std::endl;
        std::cout << "    -d interval,retries: enable watchdog and specify interval between feeding dog in ms (interval)" << std::endl;
        std::cout << "    -s static_host_url: specify the url of name server to be connected statically without host server" << std::endl;
        std::cout << "    -f: force binding to inet address even though host server is not started" << std::endl;
        std::cout << "         and maximum number of retries (retries). If '0' is given, default value will be used." << std::endl;
        return 0;
    }

    uint32_t num_interfaces = 0;
    char **interfaces_array = interfaces ? strsplit(interfaces, ",", &num_interfaces) : 0;
    uint32_t num_host_servers = 0;
    char **url_host_server_array = url_host_servers ? strsplit(url_host_servers, ",", &num_host_servers) : 0;
    uint32_t num_static_hosts = 0;
    char **url_static_host_array = url_static_hosts ? strsplit(url_static_hosts, ",", &num_static_hosts) : 0;

    int32_t wd_interval = 0;
    int32_t wd_retries = 0;
    uint32_t num_wd_params = 0;
    char **wd_params_array = watchdog_params ? strsplit(watchdog_params, ",", &num_wd_params) : 0;
    bool watchdog_enabled = false;
    if (wd_params_array)
    {
        if (num_wd_params != 2)
        {
            std::cout << "Bad watchdog parameters! please use -d interval:retries." << std::endl;
            return -1;
        }
        watchdog_enabled = true;
        wd_interval = atoi(wd_params_array[0]);
        wd_retries = atoi(wd_params_array[1]);
    }

    CFdbContext::enableNameProxy(false);
    CFdbContext::enableLogger(false);
    CNameServer *ns = new CNameServer(host_name,
                                      self_exportable_level,
                                      domain_exportable_level,
                                      upstream_exportable_level);
    if (watchdog_enabled)
    {
        std::cout << "Starting watchdog with interval " << wd_interval << " and retries " << wd_retries << std::endl;
        ns->startWatchdog(wd_interval, wd_retries);
    }
    CNameServer::COnlineParams params = {url_host_server_array,
                                         num_host_servers,
                                         interfaces_array,
                                         num_interfaces,
                                         url_static_host_array,
                                         num_static_hosts,
                                         !!force_bind_address};
    if (!ns->online(params))
    {
        return -1;
    }

    FDB_CONTEXT->start(FDB_WORKER_EXE_IN_PLACE);
    return 0;
}
