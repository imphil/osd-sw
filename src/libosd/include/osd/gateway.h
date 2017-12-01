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

#ifndef OSD_GATEWAY_H
#define OSD_GATEWAY_H

#include <osd/module.h>
#include <osd/osd.h>
#include <osd/packet.h>

#include <czmq.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup libosd-gateway Gateway
 * @ingroup libosd
 *
 * @{
 */

/**
 * Opaque context object
 *
 * This object contains all state information. Create and initialize a new
 * object with osd_gateway_new() and delete it with osd_gateway_free().
 */
struct osd_gateway_ctx;

/**
 * Read a osd_packet from the device
 *
 * @param the packet to read to (allocated by the called function)
 * @return OSD_ERROR_NOT_CONNECTED if the not connected to the device
 * @return OSD_OK if successful
 */
typedef osd_result (*packet_read_fn)(struct osd_packet **pkg);

/**
 * Write a osd_packet to the device
 *
 * @param the packet to write
 * @return OSD_ERROR_NOT_CONNECTED if the not connected to the device
 * @return OSD_OK if successful
 */
typedef osd_result (*packet_write_fn)(const struct osd_packet *pkg);

/**
 * Create new osd_gateway instance
 *
 * @param[out] ctx the osd_gateway_ctx context to be created
 * @param[in] log_ctx the log context to be used. Set to NULL to disable logging
 * @param[in] host_controller_address ZeroMQ endpoint of the host controller
 * @return OSD_OK on success, any other value indicates an error
 *
 * @see osd_gateway_free()
 */
osd_result osd_gateway_new(struct osd_gateway_ctx **ctx,
                           struct osd_log_ctx *log_ctx,
                           const char *host_controller_address,
                           uint16_t device_subnet_addr,
                           packet_read_fn packet_read,
                           packet_write_fn packet_write);

/**
 * Free and NULL a communication API context object
 *
 * Call osd_gateway_disconnect() before calling this function.
 *
 * @param ctx the osd_com context object
 */
void osd_gateway_free(struct osd_gateway_ctx **ctx);

/**
 * Connect to the host controller
 *
 * @param ctx the osd_gateway_ctx context object
 * @return OSD_OK on success, any other value indicates an error
 *
 * @see osd_gateway_disconnect()
 */
osd_result osd_gateway_connect(struct osd_gateway_ctx *ctx);

/**
 * Shut down all communication with the device
 *
 * @param ctx the osd_hostmod context object
 * @return OSD_OK on success, any other value indicates an error
 *
 * @see osd_gateway_run()
 */
osd_result osd_gateway_disconnect(struct osd_gateway_ctx *ctx);

/**
 * Is the connection to the device active?
 *
 * @param ctx the osd_hostmod context object
 * @return 1 if connected, 0 if not connected
 *
 * @see osd_gateway_connect()
 * @see osd_gateway_disconnect()
 */
bool osd_gateway_is_connected(struct osd_gateway_ctx *ctx);

/**@}*/ /* end of doxygen group libosd-gateway */

#ifdef __cplusplus
}
#endif

#endif  // OSD_GATEWAY_H
