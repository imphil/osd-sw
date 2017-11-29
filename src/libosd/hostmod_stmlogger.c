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

#include <osd/hostmod_stmlogger.h>
#include <osd/module.h>
#include <osd/osd.h>
#include <osd/reg.h>
#include "osd-private.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

/**
 * STM Logger context
 */
struct osd_hostmod_stmlogger_ctx {
    struct osd_hostmod_ctx *hostmod_ctx;
    struct osd_log_ctx *log_ctx;
    unsigned int stm_di_addr;
};

static osd_result handle_event_pkg(void *arg, struct osd_packet *pkg)
{
    osd_packet_dump(pkg, stdout);
    fflush(stdout);

    osd_packet_free(&pkg);

    return OSD_OK;
}

API_EXPORT
osd_result osd_hostmod_stmlogger_new(struct osd_hostmod_stmlogger_ctx **ctx,
                                     struct osd_log_ctx *log_ctx,
                                     const char *host_controller_address,
                                     unsigned int stm_di_addr)
{
    osd_result rv;

    struct osd_hostmod_stmlogger_ctx *c =
        calloc(1, sizeof(struct osd_hostmod_stmlogger_ctx));
    assert(c);

    c->log_ctx = log_ctx;
    c->stm_di_addr = stm_di_addr;

    struct osd_hostmod_ctx *hostmod_ctx;
    rv = osd_hostmod_new(&hostmod_ctx, log_ctx, host_controller_address,
                         handle_event_pkg, c);
    assert(OSD_SUCCEEDED(rv));
    c->hostmod_ctx = hostmod_ctx;

    *ctx = c;

    return OSD_OK;
}

static bool is_stm_module(struct osd_hostmod_stmlogger_ctx *ctx)
{
    osd_result rv;

    struct osd_module_desc desc;

    rv = osd_hostmod_describe_module(ctx->hostmod_ctx, ctx->stm_di_addr, &desc);
    if (OSD_FAILED(rv)) {
        err(ctx->log_ctx,
            "Unable to check if module %u is a STM. "
            "Assuming it is not.\n",
            ctx->stm_di_addr);
        return false;
    }

    if (desc.vendor != OSD_MODULE_VENDOR_OSD ||
        desc.type != OSD_MODULE_TYPE_STD_STM || desc.version != 0) {
        return false;
    }

    return true;
}

/**
 * Start tracing
 *
 * Instruct the STM module to start sending traces to us.
 */
API_EXPORT
osd_result osd_hostmod_stmlogger_tracestart(
    struct osd_hostmod_stmlogger_ctx *ctx)
{
    osd_result rv;
    if (!is_stm_module(ctx)) {
        err(ctx->log_ctx, "Unable to start tracing: module %u is no STM.\n",
            ctx->stm_di_addr);
    }

    uint16_t event_dest = osd_hostmod_get_diaddr(ctx->hostmod_ctx);
    rv = osd_hostmod_reg_write(ctx->hostmod_ctx, &event_dest, ctx->stm_di_addr,
                               OSD_REG_BASE_MOD_EVENT_DEST, 16, 0);
    if (OSD_FAILED(rv)) {
        return rv;
    }

    uint16_t cs = OSD_REG_BASE_MOD_CS_ACTIVE;
    rv = osd_hostmod_reg_write(ctx->hostmod_ctx, &cs, ctx->stm_di_addr,
                               OSD_REG_BASE_MOD_CS, 16, 0);
    if (OSD_FAILED(rv)) {
        return rv;
    }

    return OSD_OK;
}

API_EXPORT
osd_result osd_hostmod_stmlogger_tracestop(
    struct osd_hostmod_stmlogger_ctx *ctx)
{
    uint16_t cs = 0;
    osd_hostmod_reg_write(ctx->hostmod_ctx, &cs, ctx->stm_di_addr,
                          OSD_REG_BASE_MOD_CS, 16, 0);

    return OSD_OK;
}

API_EXPORT
osd_result osd_hostmod_stmlogger_connect(struct osd_hostmod_stmlogger_ctx *ctx)
{
    return osd_hostmod_connect(ctx->hostmod_ctx);
}

API_EXPORT
osd_result osd_hostmod_stmlogger_disconnect(
    struct osd_hostmod_stmlogger_ctx *ctx)
{
    return osd_hostmod_disconnect(ctx->hostmod_ctx);
}

API_EXPORT
void osd_hostmod_stmlogger_free(struct osd_hostmod_stmlogger_ctx **ctx_p)
{
    assert(ctx_p);
    struct osd_hostmod_stmlogger_ctx *ctx = *ctx_p;
    if (!ctx) {
        return;
    }

    osd_hostmod_free(&ctx->hostmod_ctx);

    free(ctx);
    *ctx_p = NULL;
}

API_EXPORT
struct osd_hostmod_ctx *osd_hostmod_stmlogger_get_hostmod_ctx(
    struct osd_hostmod_stmlogger_ctx *ctx)
{
    return ctx->hostmod_ctx;
}
