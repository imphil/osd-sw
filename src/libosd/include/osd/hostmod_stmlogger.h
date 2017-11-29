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

#ifndef OSD_HOSTMOD_STMLOGGER_H
#define OSD_HOSTMOD_STMLOGGER_H

#include <osd/hostmod.h>
#include <osd/osd.h>

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup libosd-hostmod-stmlogger System Trace Logger
 * @ingroup libosd
 *
 * @{
 */

struct osd_hostmod_stmlogger_ctx;

osd_result osd_hostmod_stmlogger_new(struct osd_hostmod_stmlogger_ctx **ctx,
                                     struct osd_log_ctx *log_ctx,
                                     const char *host_controller_address,
                                     unsigned int stm_di_addr);
osd_result osd_hostmod_stmlogger_connect(struct osd_hostmod_stmlogger_ctx *ctx);
osd_result osd_hostmod_stmlogger_disconnect(
    struct osd_hostmod_stmlogger_ctx *ctx);
void osd_hostmod_stmlogger_free(struct osd_hostmod_stmlogger_ctx **ctx_p);

struct osd_hostmod_ctx *osd_hostmod_stmlogger_get_hostmod_ctx(
    struct osd_hostmod_stmlogger_ctx *ctx);
osd_result osd_hostmod_stmlogger_tracestart(
    struct osd_hostmod_stmlogger_ctx *ctx);
osd_result osd_hostmod_stmlogger_tracestop(
    struct osd_hostmod_stmlogger_ctx *ctx);

/**@}*/ /* end of doxygen group libosd-hostmod-stmlogger */

#ifdef __cplusplus
}
#endif

#endif  // OSD_HOSTMOD_STMLOGGER_H
