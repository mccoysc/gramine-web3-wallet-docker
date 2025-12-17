# MySQL Group Replication Patch for SGX/Gramine

This directory contains modified MySQL Group Replication plugin source files that enable GR to work in SGX/Gramine environments where the standard `getifaddrs()` system call is not available (due to netlink socket limitations).

## Problem

MySQL Group Replication uses `getifaddrs()` to enumerate local network interfaces for:
1. Validating that the configured local address matches a local interface
2. Identifying the local node in the group configuration

In Gramine/SGX environments, `getifaddrs()` fails with `ENOSYS` because it relies on netlink sockets, which are not supported.

## Solution

We provide a custom `gr_getifaddrs()` function that replaces the standard `getifaddrs()` calls in the GR plugin. This function:

1. First checks the `GR_LOCAL_IP` environment variable for IP addresses (supports comma-separated list)
2. If not set, uses UDP socket + `getsockname()` to auto-detect the local IP
3. Returns an error if both methods fail

## Multi-IP Support

The `GR_LOCAL_IP` environment variable supports comma-separated IP addresses to enable cross-datacenter replication where both LAN and public IPs are needed:

```bash
export GR_LOCAL_IP=192.168.1.100,203.0.113.50
```

The launcher (`mysql_ratls_launcher.c`) automatically detects both LAN and public IPs and sets `GR_LOCAL_IP` with both addresses.

## Modified Files

- `gr_getifaddrs.h` - Header file with function declarations
- `gr_getifaddrs.cc` - Implementation of `gr_getifaddrs()` and `gr_freeifaddrs()`
- `sock_probe_ix.h` - Modified to use `gr_getifaddrs()` instead of `getifaddrs()`
- `recovery_endpoints.cc` - Modified to use `gr_getifaddrs()` instead of `getifaddrs()`
- `CMakeLists.txt` - Modified to include `gr_getifaddrs.cc` in the build

## MySQL Version

These patches are for MySQL 8.0.44-0ubuntu0.22.04.2 (Ubuntu package).

## Usage

The launcher automatically sets `GR_LOCAL_IP` with detected LAN and public IPs. You can also manually set it:

```bash
# Single IP
export GR_LOCAL_IP=192.168.1.100

# Multiple IPs (LAN + public)
export GR_LOCAL_IP=192.168.1.100,203.0.113.50
```

Or let the auto-detection work by not setting the variable (requires network connectivity).

## Building

The GitHub Actions workflow in this repository automatically compiles the modified GR plugin and publishes it to GitHub Releases when these files change.
