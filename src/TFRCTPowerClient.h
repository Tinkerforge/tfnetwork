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

#include "TFGenericTCPClient.h"

// configuration
#ifndef TF_RCT_POWER_CLIENT_MAX_SCHEDULED_TRANSACTION_COUNT
#define TF_RCT_POWER_CLIENT_MAX_SCHEDULED_TRANSACTION_COUNT 8
#endif

enum class TFRCTPowerClientTransactionResult
{
    Success,
    InvalidArgument,
    Aborted,
    NoTransactionAvailable,
    NotConnected,
    DisconnectedByPeer,
    SendFailed,
    ReceiveFailed,
    Timeout,
    ChecksumMismatch,
};

const char *get_tf_rct_power_client_transaction_result_name(TFRCTPowerClientTransactionResult result);

typedef std::function<void(TFRCTPowerClientTransactionResult result, float value)> TFRCTPowerClientTransactionCallback;

struct TFRCTPowerClientTransaction
{
    uint32_t id;
    micros_t timeout;
    TFRCTPowerClientTransactionCallback callback;
    TFRCTPowerClientTransaction *next;
};

class TFRCTPowerClient final : public TFGenericTCPClient
{
public:
    TFRCTPowerClient() {}

    void read(uint32_t id, micros_t timeout, TFRCTPowerClientTransactionCallback &&callback);

private:
    void close_hook() override;
    void tick_hook() override;
    bool recv_hook() override;

    void finish_pending_transaction(TFRCTPowerClientTransactionResult result, float value);
    void finish_all_transactions(TFRCTPowerClientTransactionResult result);
    void check_pending_transaction_timeout();
    void reset_pending_response();

    TFRCTPowerClientTransaction *pending_transaction        = nullptr;
    micros_t pending_transaction_deadline                   = 0_s;
    TFRCTPowerClientTransaction *scheduled_transaction_head = nullptr;
    bool wait_for_start                                     = true;
    uint8_t last_received_byte                              = 0;
    uint8_t pending_response[12];
    size_t pending_response_used                            = 0;
    uint32_t bootloader_magic_number                        = 0;
    micros_t bootloader_last_detected                       = 0_s;
};

class TFRCTPowerSharedClient final : public TFGenericTCPSharedClient
{
public:
    TFRCTPowerSharedClient(TFRCTPowerClient *client_) : TFGenericTCPSharedClient(client_), client(client_) {}

    void read(uint32_t id, micros_t timeout, TFRCTPowerClientTransactionCallback &&callback)
    {
        client->read(id, timeout, std::move(callback));
    }

private:
    TFRCTPowerClient *client;
};
