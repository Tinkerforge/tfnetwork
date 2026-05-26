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
#include <stdarg.h>
#include <stdlib.h>
#include <functional>

#if TF_NETWORK_DEBUG_LOG
#define tf_network_debugfln(fmt, ...) TFNetwork::logfln(fmt __VA_OPT__(,) __VA_ARGS__)
#else
#define tf_network_debugfln(fmt, ...) do {} while (0)
#endif

#define TF_NETWORK_IPV4_NTOA_BUFFER_LENGTH 16

typedef std::function<void(const char *fmt, va_list args)> TFNetworkVLogFLnFunction;
typedef std::function<void(uint32_t address, int error_number)> TFNetworkResolveResultCallback;
typedef std::function<void(const char *host, TFNetworkResolveResultCallback &&callback)> TFNetworkResolveFunction;
typedef std::function<uint16_t()> TFNetworkGetRandomUint16Function;

namespace TFNetwork
{
    const char *printf_safe(const char *string);

    extern TFNetworkVLogFLnFunction vlogfln;
    [[gnu::format(__printf__, 1, 2)]] void logfln(const char *fmt, ...);

    extern TFNetworkResolveFunction resolve;

    extern TFNetworkGetRandomUint16Function get_random_uint16;

    class NonReentrantScope
    {
    public:
        NonReentrantScope(bool *non_reentrant_) : non_reentrant(non_reentrant_) { *non_reentrant = true; }
        ~NonReentrantScope() { *non_reentrant = false; }

    private:
        bool *non_reentrant;
    };

    char *ipv4_ntoa(char *buffer, size_t buffer_length, uint32_t address);
};
