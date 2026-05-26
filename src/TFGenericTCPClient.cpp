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

#include "TFGenericTCPClient.h"

#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>
#include <lwip/sockets.h>

#include "TFNetwork.h"

#define debugfln(fmt, ...) tf_network_debugfln("TFGenericTCPClient[%p]::" fmt, static_cast<void *>(this) __VA_OPT__(,) __VA_ARGS__)

const char *get_tf_generic_tcp_client_connect_result_name(TFGenericTCPClientConnectResult result)
{
    switch (result) {
    case TFGenericTCPClientConnectResult::InvalidArgument:
        return "InvalidArgument";

    case TFGenericTCPClientConnectResult::NoFreePoolSlot:
        return "NoFreePoolSlot";

    case TFGenericTCPClientConnectResult::NoFreePoolShare:
        return "NoFreePoolShare";

    case TFGenericTCPClientConnectResult::NonReentrant:
        return "NonReentrant";

    case TFGenericTCPClientConnectResult::AlreadyConnected:
        return "AlreadyConnected";

    case TFGenericTCPClientConnectResult::AbortRequested:
        return "AbortRequested";

    case TFGenericTCPClientConnectResult::ResolveFailed:
        return "ResolveFailed";

    case TFGenericTCPClientConnectResult::SocketCreateFailed:
        return "SocketCreateFailed";

    case TFGenericTCPClientConnectResult::SocketGetFlagsFailed:
        return "SocketGetFlagsFailed";

    case TFGenericTCPClientConnectResult::SocketSetFlagsFailed:
        return "SocketSetFlagsFailed";

    case TFGenericTCPClientConnectResult::SocketConnectFailed:
        return "SocketConnectFailed";

    case TFGenericTCPClientConnectResult::SocketSelectFailed:
        return "SocketSelectFailed";

    case TFGenericTCPClientConnectResult::SocketGetOptionFailed:
        return "SocketGetOptionFailed";

    case TFGenericTCPClientConnectResult::SocketConnectAsyncFailed:
        return "SocketConnectAsyncFailed";

    case TFGenericTCPClientConnectResult::Timeout:
        return "Timeout";

    case TFGenericTCPClientConnectResult::Connected:
        return "Connected";
    }

    return "<Unknown>";
}

const char *get_tf_generic_tcp_client_disconnect_result_name(TFGenericTCPClientDisconnectResult result)
{
    switch (result) {
    case TFGenericTCPClientDisconnectResult::NonReentrant:
        return "NonReentrant";

    case TFGenericTCPClientDisconnectResult::NotConnected:
        return "NotConnected";

    case TFGenericTCPClientDisconnectResult::Disconnected:
        return "Disconnected";
    }

    return "<Unknown>";
}

const char *get_tf_generic_tcp_client_disconnect_reason_name(TFGenericTCPClientDisconnectReason reason)
{
    switch (reason) {
    case TFGenericTCPClientDisconnectReason::Requested:
        return "Requested";

    case TFGenericTCPClientDisconnectReason::Forced:
        return "Forced";

    case TFGenericTCPClientDisconnectReason::SocketSelectFailed:
        return "SocketSelectFailed";

    case TFGenericTCPClientDisconnectReason::SocketReceiveFailed:
        return "SocketReceiveFailed";

    case TFGenericTCPClientDisconnectReason::SocketIoctlFailed:
        return "SocketIoctlFailed";

    case TFGenericTCPClientDisconnectReason::SocketSendFailed:
        return "SocketSendFailed";

    case TFGenericTCPClientDisconnectReason::DisconnectedByPeer:
        return "DisconnectedByPeer";

    case TFGenericTCPClientDisconnectReason::ProtocolError:
        return "ProtocolError";
    }

    return "<Unknown>";
}

const char *get_tf_generic_tcp_client_connection_status_name(TFGenericTCPClientConnectionStatus status)
{
    switch (status) {
    case TFGenericTCPClientConnectionStatus::Disconnected:
        return "Disconnected";

    case TFGenericTCPClientConnectionStatus::InProgress:
        return "InProgress";

    case TFGenericTCPClientConnectionStatus::Connected:
        return "Connected";
    }

    return "<Unknown>";
}

const char *get_tf_generic_tcp_client_transfer_direction_name(TFGenericTCPClientTransferDirection direction)
{
    switch (direction) {
    case TFGenericTCPClientTransferDirection::Send:
        return "Send";

    case TFGenericTCPClientTransferDirection::Receive:
        return "Receive";
    }

    return "<Unknown>";
}

struct TFGenericTCPClientTransferHook
{
    TFGenericTCPClientTransferCallback callback;
    TFGenericTCPClientTransferHook *next;
};

TFGenericTCPClientTransferHook *TFGenericTCPClient::add_transfer_hook(TFGenericTCPClientTransferCallback &&callback)
{
    TFGenericTCPClientTransferHook *hook = new TFGenericTCPClientTransferHook;

    hook->callback = std::move(callback);
    hook->next     = transfer_hook_head;

    transfer_hook_head = hook;

    return hook;
}

bool TFGenericTCPClient::remove_transfer_hook(TFGenericTCPClientTransferHook *hook)
{
    TFGenericTCPClientTransferHook **prev_next_ptr = &transfer_hook_head;

    while (*prev_next_ptr != nullptr) {
        if (*prev_next_ptr == hook) {
            *prev_next_ptr = (*prev_next_ptr)->next;
            delete hook;

            return true;
        }

        prev_next_ptr = &(*prev_next_ptr)->next;
    }

    return false;
}

// non-reentrant
void TFGenericTCPClient::connect(const char *host, uint16_t port,
                                 TFGenericTCPClientConnectCallback &&connect_callback,
                                 TFGenericTCPClientDisconnectCallback &&disconnect_callback)
{
    if (!connect_callback) {
        debugfln("connect(host=%s port=%u) invalid argument", TFNetwork::printf_safe(host), port);
        return;
    }

    if (non_reentrant) {
        debugfln("connect(host=%s port=%u) non-reentrant", TFNetwork::printf_safe(host), port);
        connect_callback(TFGenericTCPClientConnectResult::NonReentrant, -1);
        return;
    }

    TFNetwork::NonReentrantScope scope(&non_reentrant);

    if (host == nullptr || strlen(host) == 0 || port == 0 || !disconnect_callback) {
        debugfln("connect(host=%s port=%u) invalid argument", TFNetwork::printf_safe(host), port);
        connect_callback(TFGenericTCPClientConnectResult::InvalidArgument, -1);
        return;
    }

    if (this->host != nullptr) {
        debugfln("connect(host=%s port=%u) already connected", TFNetwork::printf_safe(host), port);
        connect_callback(TFGenericTCPClientConnectResult::AlreadyConnected, -1);
        return;
    }

    debugfln("connect(host=%s port=%u) pending", TFNetwork::printf_safe(host), port);

    this->host                        = strdup(host);
    this->port                        = port;
    this->connect_callback            = std::move(connect_callback);
    this->pending_disconnect_callback = std::move(disconnect_callback);
}

// non-reentrant
TFGenericTCPClientDisconnectResult TFGenericTCPClient::disconnect()
{
    if (non_reentrant) {
        debugfln("disconnect() non-reentrant (host=%s port=%u)", TFNetwork::printf_safe(host), port);
        return TFGenericTCPClientDisconnectResult::NonReentrant;
    }

    TFNetwork::NonReentrantScope scope(&non_reentrant);

    if (host == nullptr) {
        debugfln("disconnect() not connected");
        return TFGenericTCPClientDisconnectResult::NotConnected;
    }

    debugfln("disconnect() disconnecting (host=%s port=%u)", host, port);

    TFGenericTCPClientConnectCallback connect_callback       = std::move(this->connect_callback);
    TFGenericTCPClientDisconnectCallback disconnect_callback = std::move(this->disconnect_callback);

    this->connect_callback    = nullptr;
    this->disconnect_callback = nullptr;

    close();

    if (connect_callback) { // The connect callback is not optional, but it is cleared after the connection is estabilshed
        connect_callback(TFGenericTCPClientConnectResult::AbortRequested, -1);
    }

    if (disconnect_callback) { // The disconnect callback is not optional, but it is not set until the connection is estabilshed
        disconnect_callback(TFGenericTCPClientDisconnectReason::Requested, -1);
    }

    return TFGenericTCPClientDisconnectResult::Disconnected;
}

TFGenericTCPClientConnectionStatus TFGenericTCPClient::get_connection_status() const
{
    if (socket_fd >= 0) {
        return TFGenericTCPClientConnectionStatus::Connected;
    }

    if (host != nullptr) {
        return TFGenericTCPClientConnectionStatus::InProgress;
    }

    return TFGenericTCPClientConnectionStatus::Disconnected;
}

// non-reentrant
void TFGenericTCPClient::tick()
{
    if (non_reentrant) {
        debugfln("tick() non-reentrant");
        return;
    }

    TFNetwork::NonReentrantScope scope(&non_reentrant);

    if (host == nullptr) {
        return;
    }

    tick_hook();

    if (host != nullptr && socket_fd < 0) {
        if (!resolve_pending && pending_host_address == 0 && pending_socket_fd < 0) {
            resolve_pending             = true;
            uint32_t current_resolve_id = ++resolve_id;

            debugfln("tick() resolving (host=%s current_resolve_id=%u)", host, current_resolve_id);

            TFNetwork::resolve(host,
            [this, current_resolve_id](uint32_t address, int error_number) {
                char address_str[TF_NETWORK_IPV4_NTOA_BUFFER_LENGTH];
                TFNetwork::ipv4_ntoa(address_str, sizeof(address_str), address);

                debugfln("tick() resolved (resolve_pending=%d current_resolve_id=%u resolve_id=%u address=%s error_number=%d)",
                         static_cast<int>(resolve_pending), current_resolve_id, resolve_id, address_str, error_number);

                if (!resolve_pending || current_resolve_id != resolve_id) {
                    return;
                }

                if (address == 0) {
                    abort_connect(TFGenericTCPClientConnectResult::ResolveFailed, error_number);
                    return;
                }

                resolve_pending = false;
                pending_host_address = address;
            });
        }

        if (pending_socket_fd < 0) {
            if (pending_host_address == 0) {
                return; // Waiting for resolve callback
            }

            char pending_host_address_str[TF_NETWORK_IPV4_NTOA_BUFFER_LENGTH];
            TFNetwork::ipv4_ntoa(pending_host_address_str, sizeof(pending_host_address_str), pending_host_address);

            debugfln("tick() connecting (host=%s pending_host_address=%s)", host, pending_host_address_str);
            pending_socket_fd = socket(AF_INET, SOCK_STREAM, 0);

            if (pending_socket_fd < 0) {
                abort_connect(TFGenericTCPClientConnectResult::SocketCreateFailed, errno);
                return;
            }

            int flags = fcntl(pending_socket_fd, F_GETFL, 0);

            if (flags < 0) {
                abort_connect(TFGenericTCPClientConnectResult::SocketGetFlagsFailed, errno);
                return;
            }

            if (fcntl(pending_socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                abort_connect(TFGenericTCPClientConnectResult::SocketSetFlagsFailed, errno);
                return;
            }

            struct sockaddr_in addr_in;

            memset(&addr_in, 0, sizeof(addr_in));
            memcpy(&addr_in.sin_addr.s_addr, &pending_host_address, sizeof(pending_host_address));

            addr_in.sin_family = AF_INET;
            addr_in.sin_port   = htons(port);

            pending_host_address = 0;

            if (::connect(pending_socket_fd, reinterpret_cast<struct sockaddr *>(&addr_in), sizeof(addr_in)) < 0 && errno != EINPROGRESS) {
                abort_connect(TFGenericTCPClientConnectResult::SocketConnectFailed, errno);
                return;
            }

            connect_deadline = calculate_deadline(TF_GENERIC_TCP_CLIENT_CONNECT_TIMEOUT);
        }

        if (deadline_elapsed(connect_deadline)) {
            abort_connect(TFGenericTCPClientConnectResult::Timeout, -1);
            return;
        }

        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(pending_socket_fd, &fdset);

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 0;

        int result = select(pending_socket_fd + 1, nullptr, &fdset, nullptr, &tv);

        if (result < 0) {
            abort_connect(TFGenericTCPClientConnectResult::SocketSelectFailed, errno);
            return;
        }

        if (result == 0) {
            return; // connect() in progress
        }

        if (!FD_ISSET(pending_socket_fd, &fdset)) {
            return; // connect() in progress
        }

        int socket_errno;
        socklen_t socket_errno_length = sizeof(socket_errno);

        if (getsockopt(pending_socket_fd, SOL_SOCKET, SO_ERROR, &socket_errno, &socket_errno_length) < 0) {
            abort_connect(TFGenericTCPClientConnectResult::SocketGetOptionFailed, errno);
            return;
        }

        if (socket_errno != 0) {
            abort_connect(TFGenericTCPClientConnectResult::SocketConnectAsyncFailed, socket_errno);
            return;
        }

        TFGenericTCPClientConnectCallback connect_callback = std::move(this->connect_callback);

        socket_fd                   = pending_socket_fd;
        pending_socket_fd           = -1;
        this->connect_callback      = nullptr;
        disconnect_callback         = std::move(pending_disconnect_callback);
        pending_disconnect_callback = nullptr;

        connect_callback(TFGenericTCPClientConnectResult::Connected, -1);
    }

    micros_t tick_deadline = calculate_deadline(TF_GENERIC_TCP_CLIENT_MAX_TICK_DURATION);
    bool first = true;

    while (socket_fd >= 0 && (!deadline_elapsed(tick_deadline) || first)) {
        first = false;

        if (!receive_hook()) {
            return;
        }
    }
}

void TFGenericTCPClient::close()
{
    if (pending_socket_fd >= 0) {
        ::shutdown(pending_socket_fd, SHUT_RDWR);
        ::close(pending_socket_fd);
        pending_socket_fd = -1;
    }

    if (socket_fd >= 0) {
        ::shutdown(socket_fd, SHUT_RDWR);
        ::close(socket_fd);
        socket_fd = -1;
    }

    free(host); host = nullptr;
    port = 0;
    connect_callback = nullptr;
    pending_disconnect_callback = nullptr;
    disconnect_callback = nullptr;
    resolve_pending = false;
    pending_host_address = 0;

    close_hook();
}

bool TFGenericTCPClient::send(const uint8_t *buffer, size_t length)
{
    if (length > 0 && transfer_hook_head != nullptr) {
        TFGenericTCPClientTransferHook *hook = transfer_hook_head;

        while (hook != nullptr) {
            TFGenericTCPClientTransferHook *next = hook->next;

            hook->callback(TFGenericTCPClientTransferDirection::Send, buffer, length);

            hook = next;
        }
    }

    size_t buffer_send = 0;
    size_t tries_remaining = TF_GENERIC_TCP_CLIENT_MAX_SEND_TRIES;

    while (tries_remaining > 0 && buffer_send < length) {
        --tries_remaining;

        ssize_t result = ::send(socket_fd, buffer + buffer_send, length - buffer_send, MSG_NOSIGNAL);

        if (result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }

            return false;
        }

        buffer_send += result;
    }

    return true;
}

ssize_t TFGenericTCPClient::recv(uint8_t *buffer, size_t length)
{
    ssize_t result = ::recv(socket_fd, buffer, length, 0);

    if (result > 0 && transfer_hook_head != nullptr) {
        TFGenericTCPClientTransferHook *hook = transfer_hook_head;

        while (hook != nullptr) {
            TFGenericTCPClientTransferHook *next = hook->next;

            hook->callback(TFGenericTCPClientTransferDirection::Receive, buffer, length);

            hook = next;
        }
    }

    return result;
}

void TFGenericTCPClient::abort_connect(TFGenericTCPClientConnectResult result, int error_number)
{
    TFGenericTCPClientConnectCallback connect_callback = std::move(this->connect_callback);
    this->connect_callback = nullptr;

    close();

    connect_callback(result, error_number);
}

void TFGenericTCPClient::disconnect(TFGenericTCPClientDisconnectReason reason, int error_number)
{
    TFGenericTCPClientDisconnectCallback disconnect_callback = std::move(this->disconnect_callback);
    this->disconnect_callback = nullptr;

    close();

    disconnect_callback(reason, error_number);
}
