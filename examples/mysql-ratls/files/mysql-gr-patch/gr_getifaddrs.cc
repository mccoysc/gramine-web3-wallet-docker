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

/**
 * @file gr_getifaddrs.cc
 * @brief Custom getifaddrs implementation for SGX/Gramine environments
 *
 * This module provides a replacement for the standard getifaddrs() function
 * that works in SGX/Gramine environments where netlink sockets are not
 * available.
 */

#include "xcom/gr_getifaddrs.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Environment variable name for local IP (supports comma-separated list) */
#define GR_LOCAL_IP_ENV "GR_LOCAL_IP"

/* Log prefix for debug messages */
#define LOG_PREFIX "[GR-getifaddrs] "

/* Maximum number of IPs supported in the list */
#define MAX_IPS 16

/**
 * @brief Memory block structure for gr_getifaddrs
 *
 * Each interface is allocated as a separate block containing:
 * - struct ifaddrs
 * - struct sockaddr_in for address
 * - struct sockaddr_in for netmask
 * - char array for interface name
 *
 * Multiple interfaces are linked via ifa_next pointer.
 * gr_freeifaddrs() walks the list and frees each block.
 */
struct gr_ifaddrs_block {
  struct ifaddrs ifa;
  struct sockaddr_in addr;
  struct sockaddr_in netmask;
  char name[16];
};

/**
 * @brief Try to detect local IP using UDP socket + getsockname
 *
 * This technique creates a UDP socket and "connects" it to a public IP
 * (without actually sending data). Then getsockname() returns the local
 * IP that would be used to reach that destination.
 *
 * @param[out] ip_buf Buffer to store the detected IP string
 * @param[in] buf_size Size of the buffer
 * @return 0 on success, -1 on error
 */
static int detect_ip_via_udp(char *ip_buf, size_t buf_size) {
  int sock;
  struct sockaddr_in server_addr, local_addr;
  socklen_t addr_len = sizeof(local_addr);

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    fprintf(stderr, LOG_PREFIX "Failed to create UDP socket: %s\n",
            strerror(errno));
    return -1;
  }

  /* Connect to a public DNS server (doesn't actually send data) */
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &server_addr.sin_addr);

  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    fprintf(stderr, LOG_PREFIX "Failed to connect UDP socket: %s\n",
            strerror(errno));
    close(sock);
    return -1;
  }

  /* Get the local address that would be used */
  memset(&local_addr, 0, sizeof(local_addr));
  if (getsockname(sock, (struct sockaddr *)&local_addr, &addr_len) < 0) {
    fprintf(stderr, LOG_PREFIX "Failed to get socket name: %s\n",
            strerror(errno));
    close(sock);
    return -1;
  }

  close(sock);

  if (inet_ntop(AF_INET, &local_addr.sin_addr, ip_buf, buf_size) == NULL) {
    fprintf(stderr, LOG_PREFIX "Failed to convert IP to string: %s\n",
            strerror(errno));
    return -1;
  }

  return 0;
}

/**
 * @brief Parse a single IP address (may include :port suffix)
 *
 * @param[in] ip_str The IP string (may include :port)
 * @param[out] ip_buf Buffer to store the IP string (without port)
 * @param[in] buf_size Size of the buffer
 * @return 0 on success, -1 on error
 */
static int parse_single_ip(const char *ip_str, char *ip_buf, size_t buf_size) {
  const char *colon;

  if (ip_str == NULL || strlen(ip_str) == 0) {
    return -1;
  }

  /* Skip leading whitespace */
  while (*ip_str == ' ' || *ip_str == '\t') {
    ip_str++;
  }

  /* Check for IP:port format */
  colon = strchr(ip_str, ':');
  if (colon != NULL) {
    /* Copy only the IP part */
    size_t ip_len = colon - ip_str;
    if (ip_len >= buf_size) {
      return -1;
    }
    strncpy(ip_buf, ip_str, ip_len);
    ip_buf[ip_len] = '\0';
  } else {
    /* No port, copy the whole string (trim trailing whitespace) */
    size_t len = strlen(ip_str);
    while (len > 0 && (ip_str[len - 1] == ' ' || ip_str[len - 1] == '\t')) {
      len--;
    }
    if (len >= buf_size) {
      return -1;
    }
    strncpy(ip_buf, ip_str, len);
    ip_buf[len] = '\0';
  }

  /* Validate that it's a valid IPv4 address */
  struct in_addr addr;
  if (inet_pton(AF_INET, ip_buf, &addr) != 1) {
    fprintf(stderr, LOG_PREFIX "Invalid IP address format: %s\n", ip_buf);
    return -1;
  }

  return 0;
}

/**
 * @brief Create a single ifaddrs block for an IP address
 *
 * @param[in] ip_str The IP address string
 * @param[in] if_index Interface index (for naming eth0, eth1, etc.)
 * @return Pointer to the allocated block, or NULL on error
 */
static struct gr_ifaddrs_block *create_ifaddrs_block(const char *ip_str,
                                                      int if_index) {
  struct gr_ifaddrs_block *block;
  struct in_addr addr;

  /* Convert IP string to binary */
  if (inet_pton(AF_INET, ip_str, &addr) != 1) {
    fprintf(stderr, LOG_PREFIX "Invalid IP address: %s\n", ip_str);
    return NULL;
  }

  /* Allocate the memory block */
  block = (struct gr_ifaddrs_block *)malloc(sizeof(struct gr_ifaddrs_block));
  if (block == NULL) {
    return NULL;
  }
  memset(block, 0, sizeof(struct gr_ifaddrs_block));

  /* Set up interface name (eth0, eth1, etc.) */
  snprintf(block->name, sizeof(block->name), "eth%d", if_index);

  /* Set up address */
  block->addr.sin_family = AF_INET;
  block->addr.sin_addr = addr;

  /* Set up netmask (255.255.255.0 = /24) */
  block->netmask.sin_family = AF_INET;
  inet_pton(AF_INET, "255.255.255.0", &block->netmask.sin_addr);

  /* Set up ifaddrs structure */
  block->ifa.ifa_next = NULL;
  block->ifa.ifa_name = block->name;
  block->ifa.ifa_flags = IFF_UP | IFF_RUNNING;
  block->ifa.ifa_addr = (struct sockaddr *)&block->addr;
  block->ifa.ifa_netmask = (struct sockaddr *)&block->netmask;
  block->ifa.ifa_broadaddr = NULL;
  block->ifa.ifa_data = NULL;

  fprintf(stderr, LOG_PREFIX "Created interface %s with IP %s\n", block->name,
          ip_str);

  return block;
}

int gr_getifaddrs(struct ifaddrs **ifap) {
  const char *env_ip;
  char ip_str[INET_ADDRSTRLEN];
  struct gr_ifaddrs_block *head = NULL;
  struct gr_ifaddrs_block *tail = NULL;
  int if_index = 0;
  int ip_count = 0;

  if (ifap == NULL) {
    errno = EINVAL;
    return -1;
  }

  *ifap = NULL;

  fprintf(stderr, LOG_PREFIX "gr_getifaddrs() called\n");

  /* Priority 1: Check environment variable (supports comma-separated list) */
  env_ip = getenv(GR_LOCAL_IP_ENV);
  if (env_ip != NULL && strlen(env_ip) > 0) {
    fprintf(stderr, LOG_PREFIX "Parsing %s: %s\n", GR_LOCAL_IP_ENV, env_ip);

    /* Make a copy to tokenize */
    char *env_copy = strdup(env_ip);
    if (env_copy == NULL) {
      errno = ENOMEM;
      return -1;
    }

    /* Parse comma-separated IPs */
    char *saveptr;
    char *token = strtok_r(env_copy, ",", &saveptr);
    while (token != NULL && ip_count < MAX_IPS) {
      if (parse_single_ip(token, ip_str, sizeof(ip_str)) == 0) {
        struct gr_ifaddrs_block *block = create_ifaddrs_block(ip_str, if_index);
        if (block != NULL) {
          /* Add to linked list */
          if (head == NULL) {
            head = block;
            tail = block;
          } else {
            tail->ifa.ifa_next = &block->ifa;
            tail = block;
          }
          if_index++;
          ip_count++;
        }
      } else {
        fprintf(stderr, LOG_PREFIX "Skipping invalid IP: %s\n", token);
      }
      token = strtok_r(NULL, ",", &saveptr);
    }

    free(env_copy);
  }

  /* Priority 2: If no IPs from env var, try UDP detection */
  if (ip_count == 0) {
    fprintf(stderr, LOG_PREFIX "No valid IPs from %s, trying UDP detection...\n",
            GR_LOCAL_IP_ENV);
    if (detect_ip_via_udp(ip_str, sizeof(ip_str)) == 0) {
      fprintf(stderr, LOG_PREFIX "Detected IP via UDP: %s\n", ip_str);
      struct gr_ifaddrs_block *block = create_ifaddrs_block(ip_str, 0);
      if (block != NULL) {
        head = block;
        ip_count++;
      }
    }
  }

  /* Priority 3: Return error if no IPs found */
  if (ip_count == 0) {
    fprintf(stderr, LOG_PREFIX "Failed to get any local IP addresses\n");
    errno = ENOSYS;
    return -1;
  }

  *ifap = &head->ifa;

  fprintf(stderr, LOG_PREFIX "Returning %d interface(s)\n", ip_count);

  return 0;
}

void gr_freeifaddrs(struct ifaddrs *ifa) {
  int count = 0;

  fprintf(stderr, LOG_PREFIX "gr_freeifaddrs() called\n");

  /* Walk the linked list and free each block */
  while (ifa != NULL) {
    struct ifaddrs *next = ifa->ifa_next;
    /* The ifaddrs is the first member of gr_ifaddrs_block,
       so we can directly free it */
    free(ifa);
    ifa = next;
    count++;
  }

  fprintf(stderr, LOG_PREFIX "Freed %d interface(s)\n", count);
}
