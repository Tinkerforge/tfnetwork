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

#include "TFModbusTCPServer.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <lwip/sockets.h>
#include <algorithm>

#include "TFNetwork.h"

#define debugfln(fmt, ...) tf_network_debugfln("TFModbusTCPServer[%p]::" fmt, static_cast<void *>(this) __VA_OPT__(,) __VA_ARGS__)

const char *get_tf_modbus_tcp_server_client_disconnect_reason_name(TFModbusTCPServerDisconnectReason reason)
{
    switch (reason) {
    case TFModbusTCPServerDisconnectReason::NoFreeClient:
        return "NoFreeClient";

    case TFModbusTCPServerDisconnectReason::SocketReceiveFailed:
        return "SocketReceiveFailed";

    case TFModbusTCPServerDisconnectReason::SocketSendFailed:
        return "SocketSendFailed";

    case TFModbusTCPServerDisconnectReason::DisconnectedByPeer:
        return "DisconnectedByPeer";

    case TFModbusTCPServerDisconnectReason::ProtocolError:
        return "ProtocolError";

    case TFModbusTCPServerDisconnectReason::Displaced:
        return "Displaced";

    case TFModbusTCPServerDisconnectReason::Idle:
        return "Idle";

    case TFModbusTCPServerDisconnectReason::ServerStopped:
        return "ServerStopped";
    }

    return "<Unknown>";
}

// non-reentrant
bool TFModbusTCPServer::start(uint32_t bind_address, uint16_t port,
                              TFModbusTCPServerConnectCallback &&connect_callback,
                              TFModbusTCPServerDisconnectCallback &&disconnect_callback,
                              TFModbusTCPServerRequestCallback &&request_callback)
{
    char bind_address_str[TF_NETWORK_IPV4_NTOA_BUFFER_LENGTH];
    TFNetwork::ipv4_ntoa(bind_address_str, sizeof(bind_address_str), bind_address);

    if (non_reentrant) {
        debugfln("start(bind_address=%s port=%u) non-reentrant", bind_address_str, port);

        errno = EWOULDBLOCK;
        return false;
    }

    TFNetwork::NonReentrantScope scope(&non_reentrant);

    debugfln("start(bind_address=%s port=%u)", bind_address_str, port);

    if (port == 0 || !connect_callback || !disconnect_callback || !request_callback) {
        debugfln("start(bind_address=%s port=%u) invalid argument", bind_address_str, port);

        errno = EINVAL;
        return false;
    }

    if (server_fd >= 0) {
        debugfln("start(bind_address=%s port=%u) already running", bind_address_str, port);

        errno = EBUSY;
        return false;
    }

    int pending_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (pending_fd < 0) {
        int saved_errno = errno;

        debugfln("start(bind_address=%s port=%u) socket() failed: %s (%d)",
                 bind_address_str, port, strerror(saved_errno), saved_errno);

        errno = saved_errno;
        return false;
    }

    int reuse_addr = 1;

    if (setsockopt(pending_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) < 0) {
        int saved_errno = errno;

        debugfln("start(bind_address=%s port=%u) setsockopt(SO_REUSEADDR) failed: %s (%d)",
                 bind_address_str, port, strerror(saved_errno), saved_errno);

        errno = saved_errno;
        return false;
    }

    int flags = fcntl(pending_fd, F_GETFL, 0);

    if (flags < 0) {
        int saved_errno = errno;

        debugfln("start(bind_address=%s port=%u) fcntl(F_GETFL) failed: %s (%d)",
                 bind_address_str, port, strerror(saved_errno), saved_errno);

        errno = saved_errno;
        return false;
    }

    if (fcntl(pending_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int saved_errno = errno;

        debugfln("start(bind_address=%s port=%u) fcntl(F_SETFL) failed: %s (%d)",
                 bind_address_str, port, strerror(saved_errno), saved_errno);

        errno = saved_errno;
        return false;
    }

    struct sockaddr_in addr_in;

    memset(&addr_in, 0, sizeof(addr_in));
    memcpy(&addr_in.sin_addr.s_addr, &bind_address, sizeof(bind_address));

    addr_in.sin_family = AF_INET;
    addr_in.sin_port   = htons(port);

    if (bind(pending_fd, (struct sockaddr *)&addr_in, sizeof(addr_in)) < 0) {
        int saved_errno = errno;

        debugfln("start(bind_address=%s port=%u) bind() failed: %s (%d)",
                 bind_address_str, port, strerror(saved_errno), saved_errno);

        errno = saved_errno;
        return false;
    }

    if (listen(pending_fd, 5) < 0) {
        int saved_errno = errno;

        debugfln("start(bind_address=%s port=%u) listen() failed: %s (%d)",
                 bind_address_str, port, strerror(saved_errno), saved_errno);

        errno = saved_errno;
        return false;
    }

    server_fd                 = pending_fd;
    this->connect_callback    = std::move(connect_callback);
    this->disconnect_callback = std::move(disconnect_callback);
    this->request_callback    = std::move(request_callback);

    return true;
}

// non-reentrant
bool TFModbusTCPServer::stop()
{
    if (non_reentrant) {
        debugfln("stop() non-reentrant");

        errno = EWOULDBLOCK;
        return false;
    }

    TFNetwork::NonReentrantScope scope(&non_reentrant);

    if (server_fd < 0) {
        debugfln("stop() not running");

        errno = ESRCH;
        return false;
    }

    debugfln("stop()");

    shutdown(server_fd, SHUT_RDWR);
    close(server_fd);
    server_fd = -1;

    TFModbusTCPServerClientNode *node = client_sentinel.next;
    client_sentinel.next              = nullptr;

    while (node != nullptr) {
        TFModbusTCPServerClientNode *node_next = node->next;

        disconnect(static_cast<TFModbusTCPServerClient *>(node), TFModbusTCPServerDisconnectReason::ServerStopped, -1);
        node = node_next;
    }

    connect_callback    = nullptr;
    disconnect_callback = nullptr;
    request_callback    = nullptr;

    return true;
}

// non-reentrant
void TFModbusTCPServer::tick()
{
    if (non_reentrant) {
        debugfln("tick() non-reentrant");
        return;
    }

    TFNetwork::NonReentrantScope scope(&non_reentrant);

    if (server_fd < 0) {
        return;
    }

    fd_set fdset;
    int fd_max = server_fd;

    FD_ZERO(&fdset);
    FD_SET(server_fd, &fdset);

    for (TFModbusTCPServerClientNode *node = client_sentinel.next; node != nullptr; node = node->next) {
        int socket_fd = static_cast<TFModbusTCPServerClient *>(node)->socket_fd;

        FD_SET(socket_fd, &fdset);

        fd_max = std::max(fd_max, socket_fd);
    }

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 0;

    int readable_fd_count = select(fd_max + 1, &fdset, nullptr, nullptr, &tv);

    if (readable_fd_count < 0) {
        debugfln("tick() select() failed: %s (%d)", strerror(errno), errno);
        return;
    }

    if (readable_fd_count == 0 && !deadline_elapsed(last_idle_check + TF_MODBUS_TCP_SERVER_IDLE_CHECK_INTERVAL)) {
        return;
    }

    if (readable_fd_count > 0 && FD_ISSET(server_fd, &fdset)) {
        struct sockaddr_in addr_in;
        socklen_t addr_in_length = sizeof(addr_in);
        int socket_fd            = accept(server_fd, reinterpret_cast<struct sockaddr *>(&addr_in), &addr_in_length);

        if (socket_fd < 0) {
            debugfln("tick() accept() failed: %s (%d)", strerror(errno), errno);
            return;
        }

        uint32_t peer_address = addr_in.sin_addr.s_addr;
        uint16_t port         = ntohs(addr_in.sin_port);

        char peer_address_str[TF_NETWORK_IPV4_NTOA_BUFFER_LENGTH];
        TFNetwork::ipv4_ntoa(peer_address_str, sizeof(peer_address_str), peer_address);

        debugfln("tick() accepting connection (socket_fd=%d peer_address=%s port=%u)", socket_fd, peer_address_str, port);
        connect_callback(peer_address, port);

        TFModbusTCPServerClientNode *node_prev = nullptr;
        TFModbusTCPServerClientNode *node      = &client_sentinel;
        size_t client_count                    = 0;

        while (node->next != nullptr) {
            node_prev = node;
            node      = node->next;
            ++client_count;
        }

        if (client_count >= TF_MODBUS_TCP_SERVER_MAX_CLIENT_COUNT && node != &client_sentinel) {
            TFModbusTCPServerClient *client = static_cast<TFModbusTCPServerClient *>(node);

            if (deadline_elapsed(client->last_alive + TF_MODBUS_TCP_SERVER_MIN_DISPLACE_DELAY)) {
                debugfln("tick() disconnecting client due to displacement by another connection (client=%p)", static_cast<void *>(client));

                node_prev->next = nullptr;
                --client_count;

                disconnect(client, TFModbusTCPServerDisconnectReason::Displaced, -1);
            }
        }

        if (client_count >= TF_MODBUS_TCP_SERVER_MAX_CLIENT_COUNT) {
            debugfln("tick() no free client for connection (socket_fd=%d peer_address=%s port=%u)", socket_fd, peer_address_str, port);

            shutdown(socket_fd, SHUT_RDWR);
            close(socket_fd);
            disconnect_callback(peer_address, port, TFModbusTCPServerDisconnectReason::NoFreeClient, -1);
        }
        else {
            TFModbusTCPServerClient *client = new TFModbusTCPServerClient;

            debugfln("tick() allocating client for connection (client=%p socket_fd=%d peer_address=%s port=%u)",
                     static_cast<void *>(client), socket_fd, peer_address_str, port);

            client->socket_fd                      = socket_fd;
            client->peer_address                   = peer_address;
            client->port                           = port;
            client->last_alive                     = now_us();
            client->pending_request_header_used    = 0;
            client->pending_request_header_checked = false;
            client->pending_request_payload_used   = 0;
            client->next                           = client_sentinel.next;
            client_sentinel.next                   = client;
        }
    }

    last_idle_check = now_us();

    TFModbusTCPServerClientNode *pending_head  = client_sentinel.next;
    TFModbusTCPServerClientNode *finished_head = nullptr;
    TFModbusTCPServerClientNode *finished_tail = nullptr;
    TFModbusTCPServerClientNode *node          = nullptr;

    while (true) {
        if (node != nullptr) {
            if (finished_head == nullptr) {
                finished_head = node;
                finished_tail = node;
            }
            else {
                node->next    = finished_head;
                finished_head = node;
            }
        }

        if (pending_head == nullptr) {
            break;
        }

        node         = pending_head;
        pending_head = node->next;
        node->next   = nullptr;

        TFModbusTCPServerClient *client = static_cast<TFModbusTCPServerClient *>(node);

        if (deadline_elapsed(client->last_alive + TF_MODBUS_TCP_SERVER_MAX_IDLE_DURATION)) {
            debugfln("tick() disconnecting idle client (client=%p)", static_cast<void *>(client));

            node = nullptr;
            disconnect(client, TFModbusTCPServerDisconnectReason::Idle, -1);
            continue;
        }

        if (readable_fd_count == 0 || !FD_ISSET(client->socket_fd, &fdset)) {
            if (finished_tail == nullptr) {
                finished_head = node;
                finished_tail = node;
            }
            else {
                finished_tail->next = node;
                finished_tail       = node;
            }

            node = nullptr;
            continue;
        }

        client->last_alive = now_us();

        size_t pending_request_header_missing = sizeof(client->pending_request.header) - client->pending_request_header_used;

        if (pending_request_header_missing > 0) {
            ssize_t result = recv(client->socket_fd,
                                  client->pending_request.header.bytes + client->pending_request_header_used,
                                  pending_request_header_missing,
                                  0);

            if (result < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    int saved_errno = errno;

                    debugfln("tick() disconnecting client due to receive error (client=%p errno=%d)",
                             static_cast<void *>(client), saved_errno);

                    node = nullptr;
                    disconnect(client, TFModbusTCPServerDisconnectReason::SocketReceiveFailed, saved_errno);
                }

                continue;
            }

            if (result == 0) {
                debugfln("tick() client disconnected by peer (client=%p)", static_cast<void *>(client));

                node = nullptr;
                disconnect(client, TFModbusTCPServerDisconnectReason::DisconnectedByPeer, -1);
                continue;
            }

            client->pending_request_header_used += result;
            pending_request_header_missing      -= result;

            if (pending_request_header_missing > 0) {
                continue;
            }
        }

        uint16_t frame_length = ntohs(client->pending_request.header.frame_length);

        if (!client->pending_request_header_checked) {
            uint16_t protocol_id  = ntohs(client->pending_request.header.protocol_id);

            if (protocol_id != 0) {
                debugfln("tick() disconnecting client due to protocol error, wrong protocol ID (client=%p protocol_id=%u)",
                         static_cast<void *>(client), protocol_id);

                node = nullptr;
                disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                continue;
            }

            if (frame_length < TF_MODBUS_TCP_MIN_REQUEST_FRAME_LENGTH) {
                debugfln("tick() disconnecting client due to protocol error, frame length too short (client=%p frame_length=%u)",
                         static_cast<void *>(client), frame_length);

                node = nullptr;
                disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                continue;
            }

            if (frame_length > TF_MODBUS_TCP_MAX_REQUEST_FRAME_LENGTH) {
                debugfln("tick() disconnecting client due to protocol error, frame length too long (client=%p frame_length=%u)",
                         static_cast<void *>(client), frame_length);

                node = nullptr;
                disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                continue;
            }

            client->pending_request_header_checked = true;
        }

        size_t pending_request_payload_missing = frame_length
                                               - TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                               - client->pending_request_payload_used;

        if (pending_request_payload_missing > 0) {
            ssize_t result = recv(client->socket_fd,
                                  client->pending_request.payload.bytes + client->pending_request_payload_used,
                                  pending_request_payload_missing,
                                  0);

            if (result < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    int saved_errno = errno;

                    debugfln("tick() disconnecting client due to receive error (client=%p errno=%d)",
                             static_cast<void *>(client), saved_errno);

                    node = nullptr;
                    disconnect(client, TFModbusTCPServerDisconnectReason::SocketReceiveFailed, saved_errno);
                }

                continue;
            }

            if (result == 0) {
                debugfln("tick() client disconnected by peer (client=%p)", static_cast<void *>(client));

                node = nullptr;
                disconnect(client, TFModbusTCPServerDisconnectReason::DisconnectedByPeer, -1);
                continue;
            }

            client->pending_request_payload_used += result;
            pending_request_payload_missing      -= result;

            if (pending_request_payload_missing > 0) {
                continue;
            }
        }

        TFModbusTCPExceptionCode exception_code = TFModbusTCPExceptionCode::Success;

        switch (static_cast<TFModbusTCPFunctionCode>(client->pending_request.payload.function_code)) {
        case TFModbusTCPFunctionCode::ReadCoils:
        case TFModbusTCPFunctionCode::ReadDiscreteInputs:
            {
                uint16_t expected_frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                               + offsetof(TFModbusTCPRequestPayload, byte_count);

                if (frame_length != expected_frame_length) {
                    debugfln("tick() disconnecting client due to protocol error, frame length mismatch (client=%p frame_length=%u expected_frame_length=%u)",
                             static_cast<void *>(client), frame_length, expected_frame_length);

                    node = nullptr;
                    disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                    continue;
                }

                uint16_t data_count = ntohs(client->pending_request.payload.data_count);

                if (data_count < TF_MODBUS_TCP_MIN_READ_COIL_COUNT
                 || data_count > TF_MODBUS_TCP_MAX_READ_COIL_COUNT) {
                    exception_code = TFModbusTCPExceptionCode::IllegalDataValue;
                }
                else {
                    client->response.payload.byte_count  = (data_count + 7) / 8;
                    client->response.header.frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                         + offsetof(TFModbusTCPResponsePayload, coil_values)
                                                         + client->response.payload.byte_count;

                    exception_code = request_callback(client->pending_request.header.unit_id,
                                                      static_cast<TFModbusTCPFunctionCode>(client->pending_request.payload.function_code),
                                                      ntohs(client->pending_request.payload.start_address),
                                                      data_count,
                                                      client->response.payload.coil_values);

                    client->response.payload.coil_values[client->response.payload.byte_count - 1] &= (1u << (data_count % 8)) - 1;
                }
            }

            break;

        case TFModbusTCPFunctionCode::ReadHoldingRegisters:
        case TFModbusTCPFunctionCode::ReadInputRegisters:
            {
                uint16_t expected_frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                               + offsetof(TFModbusTCPRequestPayload, byte_count);

                if (frame_length != expected_frame_length) {
                    debugfln("tick() disconnecting client due to protocol error, frame length mismatch (client=%p frame_length=%u expected_frame_length=%u)",
                             static_cast<void *>(client), frame_length, expected_frame_length);

                    node = nullptr;
                    disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                    continue;
                }

                uint16_t data_count = ntohs(client->pending_request.payload.data_count);

                if (data_count < TF_MODBUS_TCP_MIN_READ_REGISTER_COUNT
                 || data_count > TF_MODBUS_TCP_MAX_READ_REGISTER_COUNT) {
                    exception_code = TFModbusTCPExceptionCode::IllegalDataValue;
                }
                else {
                    client->response.payload.byte_count  = data_count * 2;
                    client->response.header.frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                         + offsetof(TFModbusTCPResponsePayload, register_values)
                                                         + client->response.payload.byte_count;

                    exception_code = request_callback(client->pending_request.header.unit_id,
                                                      static_cast<TFModbusTCPFunctionCode>(client->pending_request.payload.function_code),
                                                      ntohs(client->pending_request.payload.start_address),
                                                      data_count,
                                                      client->response.payload.register_values);

                    if (register_byte_order == TFModbusTCPByteOrder::Host) {
                        for (size_t i = 0; i < data_count; ++i) {
                            client->response.payload.register_values[i] = htons(client->response.payload.register_values[i]);
                        }
                    }
                }
            }

            break;

        case TFModbusTCPFunctionCode::WriteSingleCoil:
            {
                uint16_t expected_frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                               + offsetof(TFModbusTCPRequestPayload, byte_count);

                if (frame_length != expected_frame_length) {
                    debugfln("tick() disconnecting client due to protocol error, frame length mismatch (client=%p frame_length=%u expected_frame_length=%u)",
                             static_cast<void *>(client), frame_length, expected_frame_length);

                    node = nullptr;
                    disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                    continue;
                }

                uint16_t data_value = ntohs(client->pending_request.payload.data_value);

                if (data_value != 0x0000 && data_value != 0xFF00) {
                    exception_code = TFModbusTCPExceptionCode::IllegalDataValue;
                }
                else {
                    client->response.header.frame_length   = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                           + offsetof(TFModbusTCPResponsePayload, or_mask);
                    client->response.payload.start_address = client->pending_request.payload.start_address;
                    client->response.payload.data_value    = client->pending_request.payload.data_value;

                    uint8_t coil_values[1] = {static_cast<uint8_t>(data_value == 0xFF00 ? 1 : 0)};

                    exception_code = request_callback(client->pending_request.header.unit_id,
                                                      TFModbusTCPFunctionCode::WriteMultipleCoils,
                                                      ntohs(client->pending_request.payload.start_address),
                                                      1,
                                                      coil_values);
                }
            }

            break;

        case TFModbusTCPFunctionCode::WriteSingleRegister:
            {
                uint16_t expected_frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                               + offsetof(TFModbusTCPRequestPayload, byte_count);

                if (frame_length != expected_frame_length) {
                    debugfln("tick() disconnecting client due to protocol error, frame length mismatch (client=%p frame_length=%u expected_frame_length=%u)",
                             static_cast<void *>(client), frame_length, expected_frame_length);

                    node = nullptr;
                    disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                    continue;
                }

                client->response.header.frame_length   = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                       + offsetof(TFModbusTCPResponsePayload, or_mask);
                client->response.payload.start_address = client->pending_request.payload.start_address;
                client->response.payload.data_value    = client->pending_request.payload.data_value;

                uint16_t register_values[1] = {client->pending_request.payload.data_value};

                if (register_byte_order == TFModbusTCPByteOrder::Host) {
                    register_values[0] = ntohs(register_values[0]);
                }

                exception_code = request_callback(client->pending_request.header.unit_id,
                                                  TFModbusTCPFunctionCode::WriteMultipleRegisters,
                                                  ntohs(client->pending_request.payload.start_address),
                                                  1,
                                                  register_values);
            }

            break;

        case TFModbusTCPFunctionCode::WriteMultipleCoils:
            {
                uint16_t min_frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                          + offsetof(TFModbusTCPRequestPayload, coil_values)
                                          + TF_MODBUS_TCP_MIN_WRITE_COIL_BYTE_COUNT;

                if (frame_length < min_frame_length) {
                    debugfln("tick() disconnecting client due to protocol error, frame length too short (client=%p frame_length=%u min_frame_length=%u)",
                             static_cast<void *>(client), frame_length, min_frame_length);

                    node = nullptr;
                    disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                    continue;
                }

                uint16_t data_count = ntohs(client->pending_request.payload.data_count);

                if (data_count < TF_MODBUS_TCP_MIN_WRITE_COIL_COUNT
                 || data_count > TF_MODBUS_TCP_MAX_WRITE_COIL_COUNT
                 || client->pending_request.payload.byte_count != (data_count + 7) / 8) {
                    exception_code = TFModbusTCPExceptionCode::IllegalDataValue;
                }
                else {
                    uint16_t expected_frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                   + offsetof(TFModbusTCPRequestPayload, coil_values)
                                                   + client->pending_request.payload.byte_count;

                    if (frame_length != expected_frame_length) {
                        debugfln("tick() disconnecting client due to protocol error, frame length mismatch (client=%p frame_length=%u expected_frame_length=%u)",
                                 static_cast<void *>(client), frame_length, expected_frame_length);

                        node = nullptr;
                        disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                        continue;
                    }

                    client->response.header.frame_length   = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                           + offsetof(TFModbusTCPResponsePayload, or_mask);
                    client->response.payload.start_address = client->pending_request.payload.start_address;
                    client->response.payload.data_count    = client->pending_request.payload.data_count;

                    if ((data_count % 8) != 0) {
                        client->pending_request.payload.coil_values[client->pending_request.payload.byte_count - 1] &= (1u << (data_count % 8)) - 1;
                    }

                    exception_code = request_callback(client->pending_request.header.unit_id,
                                                      static_cast<TFModbusTCPFunctionCode>(client->pending_request.payload.function_code),
                                                      ntohs(client->pending_request.payload.start_address),
                                                      data_count,
                                                      client->pending_request.payload.coil_values);
                }
            }

            break;

        case TFModbusTCPFunctionCode::WriteMultipleRegisters:
            {
                uint16_t min_frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                          + offsetof(TFModbusTCPRequestPayload, register_values)
                                          + (TF_MODBUS_TCP_MIN_WRITE_REGISTER_COUNT * 2);

                if (frame_length < min_frame_length) {
                    debugfln("tick() disconnecting client due to protocol error, frame length too short (client=%p frame_length=%u min_frame_length=%u)",
                             static_cast<void *>(client), frame_length, min_frame_length);

                    node = nullptr;
                    disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                    continue;
                }

                uint16_t data_count = ntohs(client->pending_request.payload.data_count);

                if (data_count < TF_MODBUS_TCP_MIN_WRITE_REGISTER_COUNT
                 || data_count > TF_MODBUS_TCP_MAX_WRITE_REGISTER_COUNT
                 || client->pending_request.payload.byte_count != data_count * 2) {
                    exception_code = TFModbusTCPExceptionCode::IllegalDataValue;
                }
                else {
                    uint16_t expected_frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                   + offsetof(TFModbusTCPRequestPayload, register_values)
                                                   + client->pending_request.payload.byte_count;

                    if (frame_length != expected_frame_length) {
                        debugfln("tick() disconnecting client due to protocol error, frame length mismatch (client=%p frame_length=%u expected_frame_length=%u)",
                                 static_cast<void *>(client), frame_length, expected_frame_length);

                        node = nullptr;
                        disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                        continue;
                    }

                    client->response.header.frame_length   = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                           + offsetof(TFModbusTCPResponsePayload, or_mask);
                    client->response.payload.start_address = client->pending_request.payload.start_address;
                    client->response.payload.data_count    = client->pending_request.payload.data_count;

                    if (register_byte_order == TFModbusTCPByteOrder::Host) {
                        for (size_t i = 0; i < data_count; ++i) {
                            client->pending_request.payload.register_values[i] = ntohs(client->pending_request.payload.register_values[i]);
                        }
                    }

                    exception_code = request_callback(client->pending_request.header.unit_id,
                                                      static_cast<TFModbusTCPFunctionCode>(client->pending_request.payload.function_code),
                                                      ntohs(client->pending_request.payload.start_address),
                                                      data_count,
                                                      client->pending_request.payload.register_values);
                }
            }

            break;

        case TFModbusTCPFunctionCode::MaskWriteRegister:
            {
                uint16_t expected_frame_length = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                               + offsetof(TFModbusTCPRequestPayload, sentinel);

                if (frame_length != expected_frame_length) {
                    debugfln("tick() disconnecting client due to protocol error, frame length mismatch (client=%p frame_length=%u expected_frame_length=%u)",
                             static_cast<void *>(client), frame_length, expected_frame_length);

                    node = nullptr;
                    disconnect(client, TFModbusTCPServerDisconnectReason::ProtocolError, -1);
                    continue;
                }

                client->response.header.frame_length   = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                       + offsetof(TFModbusTCPResponsePayload, sentinel);
                client->response.payload.start_address = client->pending_request.payload.start_address;
                client->response.payload.and_mask      = client->pending_request.payload.and_mask;
                client->response.payload.or_mask       = client->pending_request.payload.or_mask;

                uint16_t register_values[2] = {client->pending_request.payload.and_mask, client->pending_request.payload.or_mask};

                if (register_byte_order == TFModbusTCPByteOrder::Host) {
                    register_values[0] = ntohs(register_values[0]);
                    register_values[1] = ntohs(register_values[1]);
                }

                exception_code = request_callback(client->pending_request.header.unit_id,
                                                  TFModbusTCPFunctionCode::MaskWriteRegister,
                                                  ntohs(client->pending_request.payload.start_address),
                                                  2,
                                                  register_values);
            }

            break;

        default:
            exception_code = TFModbusTCPExceptionCode::IllegalFunction;
            break;
        }

        if (exception_code != TFModbusTCPExceptionCode::ForceTimeout) {
            client->response.payload.function_code  = client->pending_request.payload.function_code;

            if (exception_code != TFModbusTCPExceptionCode::Success) {
                client->response.header.frame_length     = TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH
                                                         + offsetof(TFModbusTCPResponsePayload, exception_sentinel);
                client->response.payload.function_code  |= 0x80;
                client->response.payload.exception_code  = static_cast<uint8_t>(exception_code);
            }

            client->response.header.transaction_id = client->pending_request.header.transaction_id;
            client->response.header.protocol_id    = client->pending_request.header.protocol_id;
            client->response.header.frame_length   = htons(client->response.header.frame_length);
            client->response.header.unit_id        = client->pending_request.header.unit_id;

            if (!send_response(client)) {
                int saved_errno = errno;

                debugfln("tick() disconnecting client due to send error (client=%p errno=%d)",
                        static_cast<void *>(client), saved_errno);

                node = nullptr;
                disconnect(client, TFModbusTCPServerDisconnectReason::SocketSendFailed, saved_errno);
                continue;
            }
        }

        client->pending_request_header_used    = 0;
        client->pending_request_header_checked = false;
        client->pending_request_payload_used   = 0;
    }

    client_sentinel.next = finished_head;
}

void TFModbusTCPServer::disconnect(TFModbusTCPServerClient *client, TFModbusTCPServerDisconnectReason reason, int error_number)
{
    shutdown(client->socket_fd, SHUT_RDWR);
    close(client->socket_fd);
    disconnect_callback(client->peer_address, client->port, reason, error_number);
    delete client;
}

bool TFModbusTCPServer::send_response(TFModbusTCPServerClient *client)
{
    uint8_t *buffer        = client->response.bytes;
    size_t length          = sizeof(client->response.header) - TF_MODBUS_TCP_FRAME_IN_HEADER_LENGTH + ntohs(client->response.header.frame_length);
    size_t buffer_send     = 0;
    size_t tries_remaining = TF_MODBUS_TCP_SERVER_MAX_SEND_TRIES;

    while (tries_remaining > 0 && buffer_send < length) {
        --tries_remaining;

        ssize_t result = send(client->socket_fd, buffer + buffer_send, length - buffer_send, MSG_NOSIGNAL);

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
