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

#include <osd/gateway.h>
#include <osd/osd.h>
#include <osd/packet.h>
#include "osd-private.h"
#include "worker.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

/**
 * Gateway context
 */
struct osd_gateway_ctx {
    /** Logging context */
    struct osd_log_ctx *log_ctx;

    /** Is the gateway connected to the host controller? */
    bool is_connected_to_hostctrl;

    /** Is the gateway connected to the device? */
    bool is_connected_to_device;

    /** I/O worker */
    struct worker_ctx *ioworker_ctx;

    pthread_t devicerxthread;

    /**
     * ZeroMQ PAIR socket, forwarding data read from the device to the I/O
     * thread
     */
    zsock_t *device_rx_socket;

    /**
     * Read a single packet from the device (blocking)
     */
    packet_read_fn packet_read;
};

struct hostiothread_usr_ctx {
    /** Communication socket with the host controller */
    zsock_t *hostctrl_socket;

    /** ZeroMQ address/URL of the host controller */
    char *host_controller_address;

    /** Write a packet to the device */
    packet_write_fn packet_write;

    /**
     * ZeroMQ PAIR socket, receiving data from the device RX thread, to be
     * forwarded to the host controller
     */
    zsock_t *device_rx_socket;

    /** Address of the subnet connected to this gateway */
    uint16_t device_subnet_addr;
};

/**
 * Read data from the device encoded as Debug Transport Datagrams (DTDs)
 */
static void *devicerxthread_main(void *gateway_ctx_void)
{
    osd_result rv;
    int zmq_rv;
    struct osd_gateway_ctx *gateway_ctx = gateway_ctx_void;
    assert(gateway_ctx);

    while (1) {
        struct osd_packet *rcv_packet = NULL;
        rv = gateway_ctx->packet_read(&rcv_packet);
        if (OSD_FAILED(rv)) {
            if (rv == OSD_ERROR_NOT_CONNECTED) {
                dbg(gateway_ctx->log_ctx,
                    "Connection to device was "
                    "terminated. Aborting read thread.");
                return (void *)OSD_ERROR_NOT_CONNECTED;
            } else {
                err(gateway_ctx->log_ctx,
                    "packet_read() failed with error "
                    "%d. Trying again.",
                    rv);
                continue;
            }
        }
        assert(rcv_packet);

        zmsg_t *msg;
        msg = zmsg_new();
        assert(msg);
        zmq_rv = zmsg_addstr(msg, "D");
        assert(zmq_rv == 0);
        zmq_rv = zmsg_addmem(msg, rcv_packet->data_raw,
                             osd_packet_sizeof(rcv_packet));
        zmsg_send(&msg, gateway_ctx->device_rx_socket);
    }

    return (void *)OSD_OK;
}

/**
 * Process incoming messages from the host controller
 *
 * @return 0 if the message was processed, -1 if @p loop should be terminated
 */
static int hostiothread_rcv_from_hostctrl(zloop_t *loop, zsock_t *reader,
                                          void *thread_ctx_void)
{
    struct worker_thread_ctx *thread_ctx =
        (struct worker_thread_ctx *)thread_ctx_void;
    assert(thread_ctx);

    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);

    int retval;

    zmsg_t *msg = zmsg_recv(reader);
    if (!msg) {
        return -1;  // process was interrupted, terminate zloop
    }

    zframe_t *type_frame = zmsg_first(msg);
    assert(type_frame);
    if (zframe_streq(type_frame, "D")) {
        zframe_t *data_frame = zmsg_next(msg);
        assert(data_frame);

        struct osd_packet *pkg = (struct osd_packet *)zframe_data(data_frame);
        osd_result device_write_rv = usrctx->packet_write(pkg);
        if (OSD_FAILED(device_write_rv)) {
            if (device_write_rv == OSD_ERROR_NOT_CONNECTED) {
                err(thread_ctx->log_ctx,
                    "Connection to device was terminated.");
                // XXX: Handle this case, inform the host controller of the
                // transmission error and disconnect?
            } else {
                err(thread_ctx->log_ctx,
                    "Device write failed (%d). Packet dropped.",
                    device_write_rv);
            }
            retval = -1;
            goto free_return;
        }

    } else if (zframe_streq(type_frame, "M")) {
        assert(0 && "TODO: Handle incoming management messages.");

    } else {
        assert(0 && "Message of unknown type received.");
    }

    retval = 0;
free_return:
    zframe_destroy(&type_frame);
    zmsg_destroy(&msg);

    return retval;
}

/**
 * Send a command to the host controller
 */
static osd_result hostiothread_send_cmd(struct worker_thread_ctx *thread_ctx,
                                        const char *command)
{
    int rv;

    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);
    zsock_t *sock = usrctx->hostctrl_socket;

    // request
    zmsg_t *msg_req = zmsg_new();
    assert(msg_req);

    rv = zmsg_addstr(msg_req, "M");
    assert(rv == 0);
    rv = zmsg_addstr(msg_req, command);
    assert(rv == 0);
    rv = zmsg_send(&msg_req, sock);
    if (rv != 0) {
        err(thread_ctx->log_ctx, "Unable to send %s request to host controller",
            command);
        return OSD_ERROR_FAILURE;
    }

    // response
    errno = 0;
    zmsg_t *msg_resp = zmsg_recv(sock);
    if (!msg_resp) {
        err(thread_ctx->log_ctx,
            "No response received from host controller "
            "at %s: %s (%d)",
            usrctx->host_controller_address, strerror(errno), errno);
        return OSD_ERROR_FAILURE;
    }

    zframe_t *type_frame = zmsg_pop(msg_resp);
    assert(type_frame);
    assert(zframe_streq(type_frame, "M"));
    zframe_destroy(&type_frame);

    zframe_t *status_frame = zmsg_pop(msg_resp);
    assert(status_frame);
    if (!zframe_streq(status_frame, "ACK")) {
        char *status_str = zframe_strdup(status_frame);
        err(thread_ctx->log_ctx,
            "Received status %s as response to "
            "%s when expecting 'ACK'.",
            status_str, command);
        free(status_str);

        return OSD_ERROR_FAILURE;
    }
    zframe_destroy(&status_frame);

    zmsg_destroy(&msg_resp);

    return OSD_OK;
}

/**
 * Register as gateway for a subnet
 */
static osd_result hostiothread_register_gw(struct worker_thread_ctx *thread_ctx)
{
    int rv;
    osd_result osd_rv;

    assert(thread_ctx);
    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);

    char *command;
    rv = asprintf(&command, "GW_REGISTER %u", usrctx->device_subnet_addr);
    assert(rv != -1);

    osd_rv = hostiothread_send_cmd(thread_ctx, command);
    free(command);
    if (OSD_FAILED(osd_rv)) {
        return osd_rv;
    }

    dbg(thread_ctx->log_ctx,
        "Registered as gateway for subnet %u with "
        "host controller.",
        usrctx->device_subnet_addr);

    return OSD_OK;
}

/**
 * Unregister as gateway for a subnet
 */
static osd_result hostiothread_unregister_gw(
    struct worker_thread_ctx *thread_ctx)
{
    int rv;
    osd_result osd_rv;

    assert(thread_ctx);
    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);

    char *command;
    rv = asprintf(&command, "GW_UNREGISTER %u", usrctx->device_subnet_addr);
    assert(rv != -1);

    osd_rv = hostiothread_send_cmd(thread_ctx, command);
    free(command);
    if (OSD_FAILED(osd_rv)) {
        return osd_rv;
    }

    dbg(thread_ctx->log_ctx,
        "Unregistered as gateway for subnet %u with "
        "host controller.",
        usrctx->device_subnet_addr);

    return OSD_OK;
}

/**
 * Connect to the host controller in the I/O thread
 *
 * This function is called by the inprochelper as response to the I-CONNECT
 * message. It creates a new DIALER ZeroMQ socket and uses it to connect to the
 * host controller. After completion the function sends out a I-CONNECT-DONE
 * message. The message value is -1 if the connection failed for any reason,
 * or the DI address assigned to the host module if the connection was
 * successfully established.
 */
static void hostiothread_connect_to_hostctrl(
    struct worker_thread_ctx *thread_ctx)
{
    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);

    osd_result retval;
    osd_result osd_rv;

    // create new DIALER socket to connect with the host controller
    usrctx->hostctrl_socket = zsock_new_dealer(usrctx->host_controller_address);
    if (!usrctx->hostctrl_socket) {
        err(thread_ctx->log_ctx, "Unable to connect to %s",
            usrctx->host_controller_address);
        retval = -1;
        goto free_return;
    }
    zsock_set_rcvtimeo(usrctx->hostctrl_socket, ZMQ_RCV_TIMEOUT);

    // Register us as gateway for the device subnet
    osd_rv = hostiothread_register_gw(thread_ctx);
    if (OSD_FAILED(osd_rv)) {
        retval = -1;
        goto free_return;
    }

    // register handler for messages coming from the host controller
    int zmq_rv;
    zmq_rv = zloop_reader(thread_ctx->zloop, usrctx->hostctrl_socket,
                          hostiothread_rcv_from_hostctrl, thread_ctx);
    assert(zmq_rv == 0);
    zloop_reader_set_tolerant(thread_ctx->zloop, usrctx->hostctrl_socket);

    retval = 0;
free_return:
    if (retval == -1) {
        zsock_destroy(&usrctx->hostctrl_socket);
    }
    worker_send_status(thread_ctx->inproc_socket, "I-CONNECT-DONE", retval);
}

/**
 * Disconnect from the host controller in the I/O thread
 *
 * This function is called when receiving a I-DISCONNECT message in the
 * I/O thread. After the disconnect is done a I-DISCONNECT-DONE message is sent
 * to the main thread.
 */
static void hostiothread_disconnect_from_hostctrl(
    struct worker_thread_ctx *thread_ctx)
{
    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);

    osd_result retval;
    osd_result osd_rv;

    zloop_reader_end(thread_ctx->zloop, usrctx->hostctrl_socket);

    // Unregister us as gateway for the device subnet
    osd_rv = hostiothread_unregister_gw(thread_ctx);
    if (OSD_FAILED(osd_rv)) {
        err(thread_ctx->log_ctx,
            "Unable to unregister as gateway, continuing "
            "anyway.");
    }

    zsock_destroy(&usrctx->hostctrl_socket);

    retval = OSD_OK;

    worker_send_status(thread_ctx->inproc_socket, "I-DISCONNECT-DONE", retval);
}

static osd_result hostiothread_handle_inproc_request(
    struct worker_thread_ctx *thread_ctx, const char *name, zmsg_t *msg)
{
    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);

    if (!strcmp(name, "I-CONNECT")) {
        hostiothread_connect_to_hostctrl(thread_ctx);

    } else if (!strcmp(name, "I-DISCONNECT")) {
        hostiothread_disconnect_from_hostctrl(thread_ctx);
#if 0
    } else if (!strcmp(name, "D")) {
        // Forward data packet to the host controller
        rv = zmsg_send(&msg, usrctx->hostctrl_socket);
        assert(rv == 0);
#endif
    } else {
        assert(0 && "Received unknown message from main thread.");
    }

    zmsg_destroy(&msg);

    return OSD_OK;
}

/**
 * Handler inside the I/O worker thread: forward a packet to the host controller
 */
static int forward_devicerx_to_hostctrl(zloop_t *loop, zsock_t *reader,
                                        void *thread_ctx_void)
{
    struct worker_thread_ctx *thread_ctx = thread_ctx_void;
    assert(thread_ctx);

    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);

    int zmq_rv;

    zmsg_t *msg = zmsg_recv(reader);
    if (!msg) {
        return -1;  // process was interrupted, terminate zloop
    }

    zmq_rv = zmsg_send(&msg, usrctx->hostctrl_socket);
    assert(zmq_rv == 0);

    return 0;
}

static osd_result hostiothread_init(struct worker_thread_ctx *thread_ctx)
{
    assert(thread_ctx);
    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);

    int zmq_rv;

    usrctx->device_rx_socket = zsock_new_pair(">inproc://devicerx");
    assert(usrctx->device_rx_socket);

    zmq_rv = zloop_reader(thread_ctx->zloop, usrctx->device_rx_socket,
                          forward_devicerx_to_hostctrl, thread_ctx);
    assert(zmq_rv == 0);
    zloop_reader_set_tolerant(thread_ctx->zloop, usrctx->device_rx_socket);

    return OSD_OK;
}

static osd_result hostiothread_destroy(struct worker_thread_ctx *thread_ctx)
{
    assert(thread_ctx);
    struct hostiothread_usr_ctx *usrctx = thread_ctx->usr;
    assert(usrctx);

    zsock_destroy(&usrctx->device_rx_socket);

    free(usrctx->host_controller_address);
    free(usrctx);
    thread_ctx->usr = NULL;

    return OSD_OK;
}

API_EXPORT
osd_result osd_gateway_new(struct osd_gateway_ctx **ctx,
                           struct osd_log_ctx *log_ctx,
                           const char *host_controller_address,
                           uint16_t device_subnet_addr,
                           packet_read_fn packet_read,
                           packet_write_fn packet_write)
{
    osd_result rv;

    struct osd_gateway_ctx *c = calloc(1, sizeof(struct osd_gateway_ctx));
    assert(c);

    c->log_ctx = log_ctx;
    c->is_connected_to_hostctrl = false;
    c->is_connected_to_device = false;
    c->packet_read = packet_read;

    // prepare custom data passed to I/O thread for host communication
    struct hostiothread_usr_ctx *hostiothread_usr_data =
        calloc(1, sizeof(struct hostiothread_usr_ctx));
    assert(hostiothread_usr_data);
    hostiothread_usr_data->host_controller_address =
        strdup(host_controller_address);
    hostiothread_usr_data->packet_write = packet_write;
    hostiothread_usr_data->device_subnet_addr = device_subnet_addr;

    rv = worker_new(&c->ioworker_ctx, log_ctx, hostiothread_init,
                    hostiothread_destroy, hostiothread_handle_inproc_request,
                    hostiothread_usr_data);
    if (OSD_FAILED(rv)) {
        return rv;
    }

    *ctx = c;

    return OSD_OK;
}

static osd_result connect_to_hostctrl(struct osd_gateway_ctx *ctx)
{
    osd_result rv;

    if (ctx->is_connected_to_hostctrl) {
        return OSD_OK;
    }

    worker_send_status(ctx->ioworker_ctx->inproc_socket, "I-CONNECT", 0);
    int retval;
    rv = worker_wait_for_status(ctx->ioworker_ctx->inproc_socket,
                                "I-CONNECT-DONE", &retval);
    if (OSD_FAILED(rv) || retval == -1) {
        err(ctx->log_ctx, "Unable to establish connection to host controller.");
        return OSD_ERROR_CONNECTION_FAILED;
    }

    ctx->is_connected_to_hostctrl = true;

    dbg(ctx->log_ctx, "Connection with host controller established.");

    return OSD_OK;
}

static osd_result connect_to_device(struct osd_gateway_ctx *ctx)
{
    int irv;

    // prepare device RX thread to read data from the device and forward it to
    // the I/O thread. Forwarding is done through a inproc socket.
    ctx->device_rx_socket = zsock_new_pair("@inproc://devicerx");
    assert(ctx->device_rx_socket);

    irv = pthread_create(&ctx->devicerxthread, NULL, devicerxthread_main,
                         (void *)ctx);
    assert(irv == 0);

    ctx->is_connected_to_device = true;

    return OSD_OK;
}

API_EXPORT
osd_result osd_gateway_connect(struct osd_gateway_ctx *ctx)
{
    osd_result rv;
    assert(ctx);

    rv = connect_to_hostctrl(ctx);
    if (OSD_FAILED(rv)) {
        return rv;
    }

    rv = connect_to_device(ctx);
    if (OSD_FAILED(rv)) {
        return rv;
    }

    return OSD_OK;
}

API_EXPORT
bool osd_gateway_is_connected(struct osd_gateway_ctx *ctx)
{
    assert(ctx);
    return ctx->is_connected_to_hostctrl && ctx->is_connected_to_device;
}

static osd_result disconnect_from_hostctrl(struct osd_gateway_ctx *ctx)
{
    osd_result rv;

    assert(ctx);

    if (!ctx->is_connected_to_hostctrl) {
        return OSD_OK;
    }

    worker_send_status(ctx->ioworker_ctx->inproc_socket, "I-DISCONNECT", 0);
    osd_result retval;
    rv = worker_wait_for_status(ctx->ioworker_ctx->inproc_socket,
                                "I-DISCONNECT-DONE", &retval);
    if (OSD_FAILED(rv)) {
        return rv;
    }
    if (OSD_FAILED(retval)) {
        return retval;
    }

    ctx->is_connected_to_hostctrl = false;

    return OSD_OK;
}

static osd_result disconnect_from_device(struct osd_gateway_ctx *ctx)
{
    assert(ctx);

    if (!ctx->is_connected_to_device) {
        return OSD_OK;
    }

    // end device RX thread and associated ZeroMQ socket
    pthread_cancel(ctx->devicerxthread);
    void *retval;
    pthread_join(ctx->devicerxthread, &retval);
    if ((intptr_t)retval == OSD_OK) {
        dbg(ctx->log_ctx,
            "Device read thread terminated through cancellation.");
    } else if ((intptr_t)retval == OSD_ERROR_NOT_CONNECTED) {
        dbg(ctx->log_ctx,
            "Device read thread terminated earlier when "
            "connection was dropped.");
    }

    zsock_destroy(&ctx->device_rx_socket);

    ctx->is_connected_to_device = false;

    return OSD_OK;
}

API_EXPORT
osd_result osd_gateway_disconnect(struct osd_gateway_ctx *ctx)
{
    disconnect_from_device(ctx);
    disconnect_from_hostctrl(ctx);

    return OSD_OK;
}

API_EXPORT
void osd_gateway_free(struct osd_gateway_ctx **ctx_p)
{
    assert(ctx_p);
    struct osd_gateway_ctx *ctx = *ctx_p;
    if (!ctx) {
        return;
    }

    // Don't make these two checks fatal to clean up as much as possible
    if (ctx->is_connected_to_hostctrl) {
        info(ctx->log_ctx, "Disconnect from host controller first!");
    }
    if (ctx->is_connected_to_device) {
        info(ctx->log_ctx, "Disconnect from device first!");
    }

    worker_free(&ctx->ioworker_ctx);

    free(ctx);
    *ctx_p = NULL;
}
