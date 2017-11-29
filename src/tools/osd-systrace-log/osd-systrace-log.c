/* Copyright 2017 The Open SoC Debug Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/**
 * Open SoC Debug system trace logger
 */

#define CLI_TOOL_PROGNAME "osd-systrace-log"
#define CLI_TOOL_SHORTDESC "Open SoC Debug system trace logger"

#include <osd/hostmod_stmlogger.h>
#include "../cli-util.h"

// command line arguments
struct arg_int *a_stm_diaddr;
struct arg_str *a_hostctrl_ep;

osd_result setup(void)
{
    a_hostctrl_ep = arg_str0("e", "hostctrl", "<URL>",
                             "ZeroMQ endpoint of the host controller "
                             "(default: " DEFAULT_HOSTCTRL_EP ")");
    a_hostctrl_ep->sval[0] = DEFAULT_HOSTCTRL_EP;
    osd_tool_add_arg(a_hostctrl_ep);

    a_stm_diaddr =
        arg_int1("a", "diaddr", "<diaddr>", "DI address of the STM module");
    osd_tool_add_arg(a_stm_diaddr);

    return OSD_OK;
}

int run(void)
{
    osd_result rv;
    int exitcode;

    zsys_init();

    struct osd_log_ctx *osd_log_ctx;
    rv = osd_log_new(&osd_log_ctx, cfg.log_level, &osd_log_handler);
    assert(OSD_SUCCEEDED(rv));

    struct osd_hostmod_stmlogger_ctx *hostmod_stmlogger_ctx;
    rv = osd_hostmod_stmlogger_new(&hostmod_stmlogger_ctx, osd_log_ctx,
                                   a_hostctrl_ep->sval[0],
                                   a_stm_diaddr->ival[0]);
    assert(OSD_SUCCEEDED(rv));

    rv = osd_hostmod_stmlogger_connect(hostmod_stmlogger_ctx);
    if (OSD_FAILED(rv)) {
        fatal("Unable to connect to host controller at %s (rv=%d).\n",
              a_hostctrl_ep->sval[0], rv);
        exitcode = 1;
        goto free_return;
    }

    info("Connected to host controller at %s, starting logging.",
         a_hostctrl_ep->sval[0]);
    while (!zsys_interrupted) {
        pause();
    }
    info("Shutdown signal received, cleaning up.");

    rv = osd_hostmod_stmlogger_disconnect(hostmod_stmlogger_ctx);
    if (OSD_FAILED(rv)) {
        fatal("Unable to disconnect (%d)", rv);
        exitcode = 1;
        goto free_return;
    }

    exitcode = 0;
free_return:
    osd_hostmod_stmlogger_free(&hostmod_stmlogger_ctx);
    osd_log_free(&osd_log_ctx);
    return exitcode;
}
