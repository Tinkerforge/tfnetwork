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

#include "TFGenericTCPClientPool.h"

#include <sys/types.h>

#include "TFNetwork.h"

#define debugfln(fmt, ...) tf_network_debugfln("TFGenericTCPClientPool[%p]::" fmt, static_cast<void *>(this) __VA_OPT__(,) __VA_ARGS__)

const char *get_tf_generic_tcp_client_pool_share_level_name(TFGenericTCPClientPoolShareLevel level)
{
    switch (level) {
    case TFGenericTCPClientPoolShareLevel::Undefined:
        return "Undefined";

    case TFGenericTCPClientPoolShareLevel::Primary:
        return "Primary";

    case TFGenericTCPClientPoolShareLevel::Secondary:
        return "Secondary";
    }

    return "<Unknown>";
}

// non-reentrant
void TFGenericTCPClientPool::acquire(const char *host, uint16_t port,
                                     TFGenericTCPClientPoolConnectCallback &&connect_callback,
                                     TFGenericTCPClientPoolDisconnectCallback &&disconnect_callback)
{
    if (!connect_callback) {
        debugfln("acquire(host=%s port=%u) invalid argument", TFNetwork::printf_safe(host), port);
        return;
    }

    if (non_reentrant) {
        debugfln("acquire(host=%s port=%u) non-reentrant", TFNetwork::printf_safe(host), port);
        connect_callback(TFGenericTCPClientConnectResult::NonReentrant, -1, nullptr, TFGenericTCPClientPoolShareLevel::Undefined);
        return;
    }

    TFNetwork::NonReentrantScope scope(&non_reentrant);

    if (host == nullptr || strlen(host) == 0 || port == 0 || !disconnect_callback) {
        debugfln("acquire(host=%s port=%u) invalid argument", TFNetwork::printf_safe(host), port);
        connect_callback(TFGenericTCPClientConnectResult::InvalidArgument, -1, nullptr, TFGenericTCPClientPoolShareLevel::Undefined);
        return;
    }

    debugfln("acquire(host=%s port=%u)", host, port);

    ssize_t slot_index = -1;

    for (size_t i = 0; i < TF_GENERIC_TCP_CLIENT_POOL_MAX_SLOT_COUNT; ++i) {
        TFGenericTCPClientPoolSlot *slot = slots[i];

        if (slot == nullptr) {
            if (slot_index < 0) {
                slot_index = i;
            }

            continue;
        }

        if (slot->delete_pending) {
            if (slot_index < 0 || slots[slot_index] == nullptr) {
                slot_index = i;
            }

            continue;
        }

        debugfln("acquire(host=%s port=%u) checking existing slot (slot_index=%zu client=%p host=%s port=%u)",
                 host, port, i, static_cast<void *>(slot->client), slot->client->get_host(), slot->client->get_port());

        if (strcmp(slot->client->get_host(), host) == 0 && slot->client->get_port() == port) {
            ssize_t share_index = -1;

            debugfln("acquire(host=%s port=%u) found matching existing slot (slot_index=%zu client=%p)",
                     host, port, i, static_cast<void *>(slot->client));

            for (size_t k = 0; k < TF_GENERIC_TCP_CLIENT_POOL_MAX_SHARE_COUNT; ++k) {
                if (slot->shares[k] == nullptr) {
                    share_index = k;
                    break;
                }
            }

            if (share_index < 0) {
                connect_callback(TFGenericTCPClientConnectResult::NoFreePoolShare, -1, nullptr, TFGenericTCPClientPoolShareLevel::Undefined);
                return;
            }

            TFGenericTCPClientPoolShare *share = new TFGenericTCPClientPoolShare;
            share->shared_client = create_shared_client(slot->client);

            if (slot->client->get_connection_status() == TFGenericTCPClientConnectionStatus::Connected) {
                share->disconnect_callback = std::move(disconnect_callback);
                connect_callback(TFGenericTCPClientConnectResult::Connected, -1, share->shared_client, TFGenericTCPClientPoolShareLevel::Secondary);
            }
            else {
                share->connect_callback = std::move(connect_callback);
                share->pending_disconnect_callback = std::move(disconnect_callback);
            }

            slot->shares[share_index] = share;
            ++slot->share_count;
            return;
        }
    }

    if (slot_index < 0) {
        connect_callback(TFGenericTCPClientConnectResult::NoFreePoolSlot, -1, nullptr, TFGenericTCPClientPoolShareLevel::Undefined);
        return;
    }

    if (slots[slot_index] == nullptr) {
        slots[slot_index] = new TFGenericTCPClientPoolSlot;
    }

    TFGenericTCPClientPoolSlot *slot = slots[slot_index];

    if (slot->delete_pending) {
        debugfln("acquire(host=%s port=%u) reviving slot (slot_index=%zu slot=%p client=%p)",
                 host, port, slot_index, static_cast<void *>(slot), static_cast<void *>(slot->client));
    }

    slot->delete_pending = false;

    if (slot->client == nullptr) {
        slot->client = create_client();
    }

    debugfln("acquire(host=%s port=%u) connecting slot (slot_index=%zu slot=%p client=%p)",
             host, port, slot_index, static_cast<void *>(slot), static_cast<void *>(slot->client));

    TFGenericTCPClientPoolShare *share = new TFGenericTCPClientPoolShare;
    share->shared_client = create_shared_client(slot->client);
    share->connect_callback = std::move(connect_callback);
    share->pending_disconnect_callback = std::move(disconnect_callback);
    slot->shares[0] = share;
    ++slot->share_count;

    slot->client->connect(host, port,
    [this, slot_index](TFGenericTCPClientConnectResult result, int error_number) {
        TFGenericTCPClientPoolSlot *slot = slots[slot_index];

        debugfln("acquire(...) connected (result=%s error_number=%d slot_index=%zu slot=%p)",
                 get_tf_generic_tcp_client_connect_result_name(result), error_number,
                 slot_index, static_cast<void *>(slot));

        TFGenericTCPClientPoolShareLevel share_level = TFGenericTCPClientPoolShareLevel::Primary;

        for (size_t k = 0; k < TF_GENERIC_TCP_CLIENT_POOL_MAX_SHARE_COUNT; ++k) {
            TFGenericTCPClientPoolShare *share = slot->shares[k];

            if (share == nullptr) {
                continue;
            }

            TFGenericTCPClientPoolConnectCallback connect_callback = std::move(share->connect_callback);
            share->connect_callback = nullptr;

            if (result == TFGenericTCPClientConnectResult::Connected) {
                share->disconnect_callback = std::move(share->pending_disconnect_callback);
            }

            share->pending_disconnect_callback = nullptr;

            connect_callback(result, error_number, result == TFGenericTCPClientConnectResult::Connected ? share->shared_client : nullptr, share_level);

            share_level = TFGenericTCPClientPoolShareLevel::Secondary;

            if (result != TFGenericTCPClientConnectResult::Connected) {
                // The disconnect callback is not optional, but it is not set until the connection is
                // estabilshed. Therefore the release() call will not call the disconnect callback, hence
                // reason and error_number are unused. Pass error_number as -2 to indicate this
                release(slot_index, k, TFGenericTCPClientDisconnectReason::Requested /* unused */, -2 /* unused */, false);
            }
        }
    },
    [this, slot_index](TFGenericTCPClientDisconnectReason reason, int error_number) {
        TFGenericTCPClientPoolSlot *slot = slots[slot_index];

        if (slot->delete_pending) {
            return;
        }

        debugfln("acquire(...) disconnected (reason=%s error_number=%d slot_index=%zu slot=%p)",
                 get_tf_generic_tcp_client_disconnect_reason_name(reason), error_number,
                 slot_index, static_cast<void *>(slot));

        for (size_t k = 0; k < TF_GENERIC_TCP_CLIENT_POOL_MAX_SHARE_COUNT; ++k) {
            TFGenericTCPClientPoolShare *share = slot->shares[k];

            if (share == nullptr) {
                continue;
            }

            release(slot_index, k, reason, error_number, false);
        }
    });
}

// non-reentrant
TFGenericTCPClientDisconnectResult TFGenericTCPClientPool::release(TFGenericTCPSharedClient *shared_client, bool force_disconnect /*= false*/)
{
    if (non_reentrant) {
        debugfln("release(shared_client=%p force_disconnect=%u) non-reentrant", static_cast<void *>(shared_client), force_disconnect ? 1 : 0);
        return TFGenericTCPClientDisconnectResult::NonReentrant;
    }

    TFNetwork::NonReentrantScope scope(&non_reentrant);

    debugfln("release(shared_client=%p force_disconnect=%u)", static_cast<void *>(shared_client), force_disconnect ? 1 : 0);

    for (size_t i = 0; i < TF_GENERIC_TCP_CLIENT_POOL_MAX_SLOT_COUNT; ++i) {
        TFGenericTCPClientPoolSlot *slot = slots[i];

        if (slot == nullptr) {
            continue;
        }

        for (size_t k = 0; k < TF_GENERIC_TCP_CLIENT_POOL_MAX_SHARE_COUNT; ++k) {
            TFGenericTCPClientPoolShare *share = slot->shares[k];

            if (share == nullptr || share->shared_client != shared_client) {
                continue;
            }

            release(i, k, TFGenericTCPClientDisconnectReason::Requested, -1, true);

            if (force_disconnect) {
                for (size_t n = 0; n < TF_GENERIC_TCP_CLIENT_POOL_MAX_SHARE_COUNT; ++n) {
                    if (n == k) {
                        continue;
                    }

                    TFGenericTCPClientPoolShare *other_share = slot->shares[n];

                    if (other_share == nullptr) {
                        continue;
                    }

                    release(i, n, TFGenericTCPClientDisconnectReason::Forced, -1, true);
                }
            }

            return TFGenericTCPClientDisconnectResult::Disconnected;
        }
    }

    debugfln("release(shared_client=%p force_disconnect=%u) shared client not found", static_cast<void *>(shared_client), force_disconnect ? 1 : 0);

    return TFGenericTCPClientDisconnectResult::NotConnected;
}

// non-reentrant
void TFGenericTCPClientPool::tick()
{
    if (non_reentrant) {
        debugfln("tick() non-reentrant");
        return;
    }

    TFNetwork::NonReentrantScope scope(&non_reentrant);

    for (size_t i = 0; i < TF_GENERIC_TCP_CLIENT_POOL_MAX_SLOT_COUNT; ++i) {
        TFGenericTCPClientPoolSlot *slot = slots[i];

        if (slot == nullptr) {
            continue;
        }

        if (slot->delete_pending) {
            debugfln("tick() deleting slot (slot_index=%zu client=%p)", i, static_cast<void *>(slot->client));

            slots[i] = nullptr;
            delete slot->client;
            delete slot;
            continue;
        }

        slot->client->tick();
    }
}

void TFGenericTCPClientPool::release(size_t slot_index, size_t share_index, TFGenericTCPClientDisconnectReason reason, int error_number, bool disconnect)
{
    TFGenericTCPClientPoolSlot *slot = slots[slot_index];

    if (slot == nullptr) {
#if defined(TF_NETWORK_DEBUG_LOG) && TF_NETWORK_DEBUG_LOG > 0
        if (reason == TFGenericTCPClientDisconnectReason::Requested && error_number == -2) {
            debugfln("release(slot_index=%zu share_index=%zu disconnect=%d) invalid slot",
                     slot_index, share_index, disconnect ? 1 : 0);
        }
        else {
            debugfln("release(slot_index=%zu share_index=%zu reason=%s error_number=%d disconnect=%d) invalid slot",
                     slot_index, share_index, get_tf_generic_tcp_client_disconnect_reason_name(reason), error_number, disconnect ? 1 : 0);
        }
#endif

        return;
    }

    TFGenericTCPClientPoolShare *share = slot->shares[share_index];

    if (share == nullptr) {
#if defined(TF_NETWORK_DEBUG_LOG) && TF_NETWORK_DEBUG_LOG > 0
        if (reason == TFGenericTCPClientDisconnectReason::Requested && error_number == -2) {
            debugfln("release(slot_index=%zu share_index=%zu disconnect=%d) invalid share",
                     slot_index, share_index, disconnect ? 1 : 0);
        }
        else {
            debugfln("release(slot_index=%zu share_index=%zu reason=%s error_number=%d disconnect=%d) invalid share",
                     slot_index, share_index, get_tf_generic_tcp_client_disconnect_reason_name(reason), error_number, disconnect ? 1 : 0);
        }
#endif

        return;
    }

#if defined(TF_NETWORK_DEBUG_LOG) && TF_NETWORK_DEBUG_LOG > 0
    if (reason == TFGenericTCPClientDisconnectReason::Requested && error_number == -2) {
        debugfln("release(slot_index=%zu share_index=%zu disconnect=%d)",
                 slot_index, share_index, disconnect ? 1 : 0);
    }
    else {
        debugfln("release(slot_index=%zu share_index=%zu reason=%s error_number=%d disconnect=%d)",
                 slot_index, share_index, get_tf_generic_tcp_client_disconnect_reason_name(reason), error_number, disconnect ? 1 : 0);
    }
#endif

    slot->shares[share_index] = nullptr;
    --slot->share_count;

    TFGenericTCPClientPoolDisconnectCallback disconnect_callback = std::move(share->disconnect_callback);
    share->disconnect_callback = nullptr;

    if (disconnect_callback != nullptr) { // The disconnect callback is not optional, but it is not set until the connection is estabilshed
        disconnect_callback(reason, error_number, share->shared_client, slot->share_count == 0 ? TFGenericTCPClientPoolShareLevel::Primary : TFGenericTCPClientPoolShareLevel::Secondary);
    }

    delete share->shared_client;
    delete share;

    if (slot->share_count == 0) {
#if defined(TF_NETWORK_DEBUG_LOG) && TF_NETWORK_DEBUG_LOG > 0
        if (reason == TFGenericTCPClientDisconnectReason::Requested && error_number == -2) {
            debugfln("release(slot_index=%zu share_index=%zu reason=%s error_number=%d disconnect=%d) marking inactive slot for deletion (client=%p host=%s port=%u)",
                     slot_index, share_index, get_tf_generic_tcp_client_disconnect_reason_name(reason), error_number, disconnect ? 1 : 0,
                     static_cast<void *>(slot->client), TFNetwork::printf_safe(slot->client->get_host()), slot->client->get_port());
        }
        else {
            debugfln("release(slot_index=%zu share_index=%zu disconnect=%d) marking inactive slot for deletion (client=%p host=%s port=%u)",
                     slot_index, share_index, disconnect ? 1 : 0,
                     static_cast<void *>(slot->client), TFNetwork::printf_safe(slot->client->get_host()), slot->client->get_port());
        }
#endif

        slot->delete_pending = true;

        if (disconnect) {
            slot->client->disconnect();
        }
    }
}
