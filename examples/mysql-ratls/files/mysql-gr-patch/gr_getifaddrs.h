/* Copyright (c) 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef GR_GETIFADDRS_H
#define GR_GETIFADDRS_H

/**
 * @file gr_getifaddrs.h
 * @brief Custom getifaddrs implementation for SGX/Gramine environments
 *
 * This module provides a replacement for the standard getifaddrs() function
 * that works in SGX/Gramine environments where netlink sockets are not
 * available.
 *
 * The implementation tries the following sources in order:
 * 1. GR_LOCAL_IP environment variable (if set)
 * 2. UDP socket + getsockname() auto-detection
 * 3. Returns error if both fail
 *
 * Usage: Replace calls to getifaddrs() with gr_getifaddrs() and
 *        freeifaddrs() with gr_freeifaddrs() in the GR plugin code.
 */

#include <ifaddrs.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get network interface addresses (SGX/Gramine compatible)
 *
 * This function provides a replacement for getifaddrs() that works in
 * SGX/Gramine environments. It returns a single interface with the IP
 * address obtained from the GR_LOCAL_IP environment variable or via
 * UDP socket detection.
 *
 * @param[out] ifap Pointer to store the interface list
 * @return 0 on success, -1 on error (with errno set)
 */
int gr_getifaddrs(struct ifaddrs **ifap);

/**
 * @brief Free interface addresses allocated by gr_getifaddrs()
 *
 * This function frees the memory allocated by gr_getifaddrs().
 *
 * @param ifa Pointer to the interface list to free
 */
void gr_freeifaddrs(struct ifaddrs *ifa);

#ifdef __cplusplus
}
#endif

#endif /* GR_GETIFADDRS_H */
