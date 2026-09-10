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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <sys/random.h>
#include <Arduino.h>
#include "../src/TFNetwork.h"
#include "../src/TFModbusTCPClient.h"
#include "../src/TFModbusTCPClientPool.h"

micros_t now_us()
{
    struct timeval tv;
    static int64_t baseline_sec = 0;

    gettimeofday(&tv, nullptr);

    if (baseline_sec == 0) {
        baseline_sec = tv.tv_sec;
    }

    return micros_t{(static_cast<int64_t>(tv.tv_sec) - baseline_sec) * 1000000 + tv.tv_usec};
}

static volatile bool running = true;

void sigint_handler(int dummy)
{
    (void)dummy;

    TFNetwork::logfln("received SIGINT");

    running = false;
}

int main()
{
    TFNetwork::vlogfln =
    [](const char *format, va_list args) {
        printf("%li | ", (int64_t)now_us());
        vprintf(format, args);
        puts("");
    };

    TFNetwork::get_random_uint16 =
    []() {
        uint16_t r;

        if (getrandom(&r, sizeof(r), 0) != sizeof(r)) {
            abort();
        }

        return r;
    };

    signal(SIGINT, sigint_handler);

    uint16_t read_register_buffer[2] = {0, 0};
    uint16_t write_register_buffer;
    uint8_t read_coil_buffer[2] = {0, 0};
    uint8_t write_coil_buffer;
    char *resolve_host = nullptr;
    std::function<void(ip_addr_t *address, int error_number)> resolve_callback;
    TFModbusTCPClient client(TFModbusTCPByteOrder::Host);
    micros_t next_read_time = -1_s;
    micros_t next_reconnect;

    TFNetwork::resolve =
    [&resolve_host, &resolve_callback](const char *host, std::function<void(ip_addr_t *address, int error_number)> &&callback) {
        resolve_host = strdup(host);
        resolve_callback = std::move(callback);
    };

    TFNetwork::logfln(" connect...");
    client.connect("localhost", 502,
    [&next_read_time](TFGenericTCPClientConnectResult result, int error_number) {
        TFNetwork::logfln("connect 1st: %s / %s (%d)",
                          get_tf_generic_tcp_client_connect_result_name(result),
                          strerror(error_number),
                          error_number);

        if (result == TFGenericTCPClientConnectResult::Connected) {
            next_read_time = calculate_deadline(100_ms);
        }
        else {
            //running = false;
        }
    },
    [](TFGenericTCPClientDisconnectReason reason, int error_number) {
        TFNetwork::logfln("disconnect 1st: %s / %s (%d)",
                          get_tf_generic_tcp_client_disconnect_reason_name(reason),
                          strerror(error_number),
                          error_number);

        //running = false;
    });

    next_reconnect = calculate_deadline(5_s);

    while (running) {
        if (resolve_host != nullptr && resolve_callback) {
            hostent *result = gethostbyname(resolve_host);

            free(resolve_host);
            resolve_host = nullptr;

            if (result == nullptr) {
                resolve_callback(nullptr, h_errno);
                resolve_callback = nullptr;
                continue;
            }

            ip_addr_t address;
            ip_addr_set_ip4_u32_val(address, ((struct in_addr *)result->h_addr)->s_addr);
            resolve_callback(&address, 0);
            resolve_callback = nullptr;
        }

        if (next_read_time >= 0_s && deadline_elapsed(next_read_time)) {
            next_read_time = -1_s;

            TFNetwork::logfln("read input registers...");
            client.transact(1, TFModbusTCPFunctionCode::ReadInputRegisters, 1013, 2, read_register_buffer, 1_s,
            [&read_register_buffer](TFModbusTCPClientTransactionResult result, const char *error_message) {
                union {
                    float f;
                    uint16_t r[2];
                } c32;

                c32.r[0] = read_register_buffer[0];
                c32.r[1] = read_register_buffer[1];

                TFNetwork::logfln("read input registers: %s (%d)%s%s [%u %u -> %f]",
                                  get_tf_modbus_tcp_client_transaction_result_name(result),
                                  static_cast<int>(result),
                                  error_message != nullptr ? " / " : "",
                                  error_message != nullptr ? error_message : "",
                                  c32.r[0],
                                  c32.r[1],
                                  static_cast<double>(c32.f));
            });

            TFNetwork::logfln("read coils...");
            client.transact(1, TFModbusTCPFunctionCode::ReadCoils, 122, 10, read_coil_buffer, 1_s,
            [&next_read_time, &read_coil_buffer](TFModbusTCPClientTransactionResult result, const char *error_message) {
                TFNetwork::logfln("read coils: %s (%d)%s%s [%u %u %u %u %u %u %u %u %u %u]",
                                  get_tf_modbus_tcp_client_transaction_result_name(result),
                                  static_cast<int>(result),
                                  error_message != nullptr ? " / " : "",
                                  error_message != nullptr ? error_message : "",
                                  (read_coil_buffer[0] >> 0) & 1,
                                  (read_coil_buffer[0] >> 1) & 1,
                                  (read_coil_buffer[0] >> 2) & 1,
                                  (read_coil_buffer[0] >> 3) & 1,
                                  (read_coil_buffer[0] >> 4) & 1,
                                  (read_coil_buffer[0] >> 5) & 1,
                                  (read_coil_buffer[0] >> 6) & 1,
                                  (read_coil_buffer[0] >> 7) & 1,
                                  (read_coil_buffer[1] >> 0) & 1,
                                  (read_coil_buffer[1] >> 1) & 1);

                next_read_time = calculate_deadline(100_ms);
            });

            write_register_buffer = 5678;

            TFNetwork::logfln("write register...");
            client.transact(1, TFModbusTCPFunctionCode::WriteSingleRegister, 2233, 1, &write_register_buffer, 1_s,
            [](TFModbusTCPClientTransactionResult result, const char *error_message) {
                TFNetwork::logfln("write register: %s (%d)%s%s",
                                  get_tf_modbus_tcp_client_transaction_result_name(result),
                                  static_cast<int>(result),
                                  error_message != nullptr ? " / " : "",
                                  error_message != nullptr ? error_message : "");
            });

            write_coil_buffer = 1;

            TFNetwork::logfln("write coil...");
            client.transact(1, TFModbusTCPFunctionCode::WriteSingleCoil, 4567, 1, &write_coil_buffer, 1_s,
            [](TFModbusTCPClientTransactionResult result, const char *error_message) {
                TFNetwork::logfln("write coil: %s (%d)%s%s",
                                  get_tf_modbus_tcp_client_transaction_result_name(result),
                                  static_cast<int>(result),
                                  error_message != nullptr ? " / " : "",
                                  error_message != nullptr ? error_message : "");
            });
        }

        if (next_reconnect >= 0_s && deadline_elapsed(next_reconnect)) {
            next_reconnect = -1_s;

            TFNetwork::logfln("disconnect...");
            client.disconnect();

            TFNetwork::logfln("reconnect...");
            client.connect("localhost", 502,
            [&next_read_time](TFGenericTCPClientConnectResult result, int error_number) {
                TFNetwork::logfln("connect 2nd: %s / %s (%d)",
                                  get_tf_generic_tcp_client_connect_result_name(result),
                                  strerror(error_number),
                                  error_number);

                if (result == TFGenericTCPClientConnectResult::Connected) {
                    next_read_time = calculate_deadline(100_ms);
                }
                else {
                    //running = false;
                }
            },
            [](TFGenericTCPClientDisconnectReason reason, int error_number) {
                TFNetwork::logfln("disconnect 2nd: %s / %s (%d)",
                                  get_tf_generic_tcp_client_disconnect_reason_name(reason),
                                  strerror(error_number),
                                  error_number);

                //running = false;
            });
        }

        client.tick();
        usleep(100);
    }

    client.disconnect();
    client.tick();

    return 0;
}
