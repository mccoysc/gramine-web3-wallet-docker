# MySQL Group Replication Patch for SGX/Gramine

This directory contains modified MySQL Group Replication plugin source files that enable GR to work in SGX/Gramine environments. The patches address two main issues:

1. **Network Interface Enumeration**: The standard `getifaddrs()` system call is not available in Gramine/SGX due to netlink socket limitations.
2. **SSL CA Configuration**: MySQL unconditionally passes an empty `ca_file` parameter to XCom SSL initialization, causing failures when using self-signed certificates without a CA (e.g., RA-TLS).

## Problem 1: getifaddrs() Not Available

MySQL Group Replication uses `getifaddrs()` to enumerate local network interfaces for:
1. Validating that the configured local address matches a local interface
2. Identifying the local node in the group configuration

In Gramine/SGX environments, `getifaddrs()` fails with `ENOSYS` because it relies on netlink sockets, which are not supported.

## Problem 2: SSL CA Empty String Bug

When SSL is enabled for Group Replication (`group_replication_ssl_mode` > DISABLED), MySQL's `plugin.cc` unconditionally adds the `ca_file` parameter to the GCS module parameters, even when `ssl_ca` is not configured (empty string). This causes the XCom SSL initialization to fail with:

```
[GCS] Failed to locate and verify ca_file:  ca_path: NULL
[GCS] Cannot use default locations because ca_file or ca_path has been specified
[GCS] Error initializing SSL
```

The root cause is in `plugin.cc` line ~2715:
```cpp
gcs_module_parameters.add_parameter("ca_file", ssl_ca);  // Always added, even if empty
if (!ssl_capath.empty())
  gcs_module_parameters.add_parameter("ca_path", ssl_capath);  // Only added if not empty
```

When `ssl_ca` is empty, `SSL_CTX_load_verify_locations(ssl_ctx, "", NULL)` fails, and since `ca_file` is a non-NULL pointer (empty string), the error path is triggered instead of falling back to default CA locations.

## Solution 1: Custom gr_getifaddrs()

We provide a custom `gr_getifaddrs()` function that replaces the standard `getifaddrs()` calls in the GR plugin. This function:

1. First checks the `GR_LOCAL_IP` environment variable for IP addresses (supports comma-separated list)
2. If not set, uses UDP socket + `getsockname()` to auto-detect the local IP
3. Returns an error if both methods fail

## Solution 2: SSL CA Empty String Fix

We patch `plugin.cc` to check if `ssl_ca` is empty before adding the `ca_file` parameter, consistent with how `ssl_capath` is handled:

```cpp
// Before (bug):
gcs_module_parameters.add_parameter("ca_file", ssl_ca);

// After (fix):
if (!ssl_ca.empty())
  gcs_module_parameters.add_parameter("ca_file", ssl_ca);
```

This allows MySQL Group Replication to work with self-signed certificates (like RA-TLS) where no CA certificate is configured. When `ca_file` is not added, the XCom SSL initialization falls back to using default CA locations or skips CA verification entirely (depending on `ssl_mode`).

## Multi-IP Support

The `GR_LOCAL_IP` environment variable supports comma-separated IP addresses to enable cross-datacenter replication where both LAN and public IPs are needed:

```bash
export GR_LOCAL_IP=192.168.1.100,203.0.113.50
```

The launcher (`mysql_ratls_launcher.c`) automatically detects both LAN and public IPs and sets `GR_LOCAL_IP` with both addresses.

## Modified Files

### For getifaddrs() fix:
- `gr_getifaddrs.h` - Header file with function declarations
- `gr_getifaddrs.cc` - Implementation of `gr_getifaddrs()` and `gr_freeifaddrs()`
- `sock_probe_ix.h` - Modified to use `gr_getifaddrs()` instead of `getifaddrs()`
- `recovery_endpoints.cc` - Modified to use `gr_getifaddrs()` instead of `getifaddrs()`
- `CMakeLists.txt` - Modified to include `gr_getifaddrs.cc` in the build

### For SSL CA empty string fix:
- `plugin_ssl_ca_fix.patch` - Patch file for `plugin/group_replication/src/plugin.cc` to fix the SSL CA empty string bug

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
