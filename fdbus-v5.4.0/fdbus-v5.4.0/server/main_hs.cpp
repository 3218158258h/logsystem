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

#include <fdbus/CFdbContext.h>
#include "CHostServer.h"
#include <fdbus/fdb_option_parser.h>
#include <iostream>

using namespace ipc::fdbus;

int main(int argc, char **argv)
{
    int32_t help = 0;
    char *upstream_hosts = 0;
    char *domain_name = 0;
    int32_t domain_exportable_level = -1;
    int32_t upstream_exportable_level = -1;
    int32_t self_exportable_level = -1;
	const struct fdb_option core_options[] = {
        { FDB_OPTION_STRING, "upstream", 'u', &upstream_hosts },
        { FDB_OPTION_STRING, "domain_name", 'n', &domain_name },
        { FDB_OPTION_INTEGER, "domain_level", 'o', &domain_exportable_level },
        { FDB_OPTION_INTEGER, "upstream_level", 'p', &upstream_exportable_level },
        { FDB_OPTION_INTEGER, "self_level", 'l', &self_exportable_level },
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
        std::cout << "Usage: host_server" << std::endl;
        std::cout << "Start host server in case of multi-host." << std::endl;
        std::cout << "Note that only one instance is needed to run for a multi-domain system." << std::endl;
        std::cout << "Usage: host_server[ -n domain_name][ -u host_url1,host_url2...][-l exportable level][-p upstream exportable level][-o domain exportable level]" << std::endl;
        std::cout << "    -u: specify urls of upstream hosts separated by ','" << std::endl;
        std::cout << "    -n: specify domain name" << std::endl;
        std::cout << "    -l self_level: specify exportable level of host server" << std::endl;
        std::cout << "    -o domain_level: specify the maximum level that should be exported within the domain" << std::endl;
        std::cout << "    -p upstream_level: specify the minimum level that should be exported to upstream host server" << std::endl;
        return 0;
    }

    uint32_t num_upstream_hosts = 0;
    char **upstream_host_array = upstream_hosts ? strsplit(upstream_hosts, ",", &num_upstream_hosts) : 0;
    CFdbContext::enableLogger(false);
    CHostServer *hs = new CHostServer(domain_name,
                                      self_exportable_level,
                                      domain_exportable_level,
                                      upstream_exportable_level);
    if (!hs)
    {
        return 0;
    }

    hs->online(upstream_host_array, num_upstream_hosts);
    FDB_CONTEXT->start(FDB_WORKER_EXE_IN_PLACE);
    return 0;
}
