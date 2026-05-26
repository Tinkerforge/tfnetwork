/* TFNetwork
 * Copyright (C) 2024 Matthias Bolte <matthias@tinkerforge.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <functional>
#include <TFTools/Micros.h>

// configuration
#ifndef TF_GENERIC_TCP_CLIENT_MAX_TICK_DURATION
#define TF_GENERIC_TCP_CLIENT_MAX_TICK_DURATION 10_ms
#endif

#ifndef TF_GENERIC_TCP_CLIENT_CONNECT_TIMEOUT
#define TF_GENERIC_TCP_CLIENT_CONNECT_TIMEOUT   3_s
#endif

#ifndef TF_GENERIC_TCP_CLIENT_MAX_SEND_TRIES
#define TF_GENERIC_TCP_CLIENT_MAX_SEND_TRIES    10
#endif

enum class TFGenericTCPClientConnectResult
{
    InvalidArgument,
    NoFreePoolSlot,
    NoFreePoolShare,
    NonReentrant,
    AlreadyConnected,
    AbortRequested,
    ResolveFailed,            // errno as received from resolve callback
    SocketCreateFailed,       // errno
    SocketGetFlagsFailed,     // errno
    SocketSetFlagsFailed,     // errno
    SocketConnectFailed,      // errno
    SocketSelectFailed,       // errno
    SocketGetOptionFailed,    // errno
    SocketConnectAsyncFailed, // errno
    Timeout,
    Connected,
};

const char *get_tf_generic_tcp_client_connect_result_name(TFGenericTCPClientConnectResult result);

enum class TFGenericTCPClientDisconnectResult
{
    NonReentrant,
    NotConnected,
    Disconnected,
};

const char *get_tf_generic_tcp_client_disconnect_result_name(TFGenericTCPClientDisconnectResult result);

enum class TFGenericTCPClientDisconnectReason
{
    Requested,
    Forced,
    SocketSelectFailed,  // errno
    SocketReceiveFailed, // errno
    SocketIoctlFailed,   // errno
    SocketSendFailed,    // errno
    DisconnectedByPeer,
    ProtocolError,
};

const char *get_tf_generic_tcp_client_disconnect_reason_name(TFGenericTCPClientDisconnectReason reason);

enum class TFGenericTCPClientConnectionStatus
{
    Disconnected,
    InProgress,
    Connected,
};

const char *get_tf_generic_tcp_client_connection_status_name(TFGenericTCPClientConnectionStatus status);

enum class TFGenericTCPClientTransferDirection
{
    Send,
    Receive,
};

const char *get_tf_generic_tcp_client_transfer_direction_name(TFGenericTCPClientTransferDirection direction);

typedef std::function<void(TFGenericTCPClientTransferDirection direction, const uint8_t *buffer, size_t length)> TFGenericTCPClientTransferCallback;
typedef std::function<void(TFGenericTCPClientConnectResult result, int error_number)> TFGenericTCPClientConnectCallback;
typedef std::function<void(TFGenericTCPClientDisconnectReason reason, int error_number)> TFGenericTCPClientDisconnectCallback;

struct TFGenericTCPClientTransferHook;

class TFGenericTCPClient
{
public:
    TFGenericTCPClient() {}
    virtual ~TFGenericTCPClient() {}

    TFGenericTCPClient(TFGenericTCPClient const &other) = delete;
    TFGenericTCPClient &operator=(TFGenericTCPClient const &other) = delete;

    TFGenericTCPClientTransferHook *add_transfer_hook(TFGenericTCPClientTransferCallback &&callback);
    bool remove_transfer_hook(TFGenericTCPClientTransferHook *hook);
    void connect(const char *host, uint16_t port, TFGenericTCPClientConnectCallback &&connect_callback,
                 TFGenericTCPClientDisconnectCallback &&disconnect_callback); // non-reentrant
    TFGenericTCPClientDisconnectResult disconnect(); // non-reentrant
    const char *get_host() const { return host; }
    uint16_t get_port() const { return port; }
    TFGenericTCPClientConnectionStatus get_connection_status() const;
    void tick(); // non-reentrant

protected:
    virtual void close_hook()   = 0;
    virtual void tick_hook()    = 0;
    virtual bool receive_hook() = 0;

    void close();
    bool send(const uint8_t *buffer, size_t length);
    ssize_t recv(uint8_t *buffer, size_t length);
    void abort_connect(TFGenericTCPClientConnectResult result, int error_number);
    void disconnect(TFGenericTCPClientDisconnectReason reason, int error_number);

    TFGenericTCPClientTransferHook *transfer_hook_head = nullptr;
    bool non_reentrant            = false;
    char *host                    = nullptr;
    uint16_t port                 = 0;
    TFGenericTCPClientConnectCallback connect_callback;
    TFGenericTCPClientDisconnectCallback pending_disconnect_callback;
    TFGenericTCPClientDisconnectCallback disconnect_callback;
    bool resolve_pending          = false;
    uint32_t resolve_id           = 0;
    uint32_t pending_host_address = 0; // IPv4 only
    int pending_socket_fd         = -1;
    micros_t connect_deadline     = 0_s;
    int socket_fd                 = -1;
};

class TFGenericTCPSharedClient
{
public:
    TFGenericTCPSharedClient(TFGenericTCPClient *client_) : client(client_) {}
    virtual ~TFGenericTCPSharedClient() {}

    TFGenericTCPSharedClient(TFGenericTCPSharedClient const &other) = delete;
    TFGenericTCPSharedClient &operator=(TFGenericTCPSharedClient const &other) = delete;

    TFGenericTCPClientTransferHook *add_transfer_hook(TFGenericTCPClientTransferCallback &&callback) { return client->add_transfer_hook(std::move(callback)); }
    bool remove_transfer_hook(TFGenericTCPClientTransferHook *hook) { return client->remove_transfer_hook(hook); }
    const char *get_host() const { return client->get_host(); }
    uint16_t get_port() const { return client->get_port(); }
    TFGenericTCPClientConnectionStatus get_connection_status() const { return client->get_connection_status(); }

private:
    TFGenericTCPClient *client;
};
