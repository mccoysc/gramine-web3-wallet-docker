# MySQL Group Replication Patch for SGX/Gramine

This directory contains modified MySQL Group Replication plugin source files that enable GR to work in SGX/Gramine environments. The patches address four main issues:

1. **Network Interface Enumeration**: The standard `getifaddrs()` system call is not available in Gramine/SGX due to netlink socket limitations.
2. **SSL CA Configuration**: MySQL unconditionally passes an empty `ca_file` parameter to XCom SSL initialization, causing failures when using self-signed certificates without a CA (e.g., RA-TLS).
3. **GCS Debug Trace Path**: The GCS_DEBUG_TRACE file is hardcoded to be written in the MySQL data directory, which is in an encrypted partition in SGX environments and cannot be read from outside.
4. **XCom SSL Verify Mode**: When `group_replication_ssl_mode=REQUIRED`, MySQL sets `SSL_VERIFY_NONE` for both server and client SSL contexts, which prevents mutual TLS certificate exchange required for RA-TLS.

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

## Problem 3: XCom SSL Verify Mode

When `group_replication_ssl_mode=REQUIRED`, MySQL's XCom network provider sets `SSL_VERIFY_NONE` for both server and client SSL contexts. This means:

1. **Server context**: Does not request client certificates (no `CertificateRequest` sent)
2. **Client context**: Does not verify server certificates (verify callback not triggered)

This is problematic for RA-TLS (Remote Attestation TLS) deployments where:
- Both sides need to exchange certificates containing SGX quotes
- The RA-TLS verification callback must be triggered to verify SGX quotes
- Mutual TLS is required for bidirectional attestation

## Solution 3: Conditional SSL_VERIFY_PEER

We patch `xcom_network_provider_ssl_native_lib.cc` to enable `SSL_VERIFY_PEER` when client certificates are configured:

```cpp
// Before (SSL_REQUIRED mode):
verify_server = SSL_VERIFY_NONE;
verify_client = SSL_VERIFY_NONE;

// After (when client_cert_file is configured):
if (ssl_mode != SSL_REQUIRED || (client_cert_file && client_cert_file[0] != '\0'))
    verify_server = SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE;
if (ssl_mode != SSL_REQUIRED || (client_cert_file && client_cert_file[0] != '\0'))
    verify_client = SSL_VERIFY_PEER;
```

This fix:
- Preserves original `SSL_REQUIRED` semantics (encryption-only) for non-RA-TLS deployments
- Enables mutual TLS when client certificates are configured (RA-TLS scenario)
- Ensures the server requests client certificates and triggers verification callbacks

## Problem 4: GCS Debug Trace Path

When `group_replication_communication_debug_options` includes `GCS_DEBUG_TRACE`, MySQL writes debug trace information to a file named `GCS_DEBUG_TRACE` in the MySQL data directory. In SGX/Gramine environments, the data directory is typically in an encrypted partition, making the debug trace file unreadable from outside the enclave.

## Solution 4: Configurable GCS Debug Trace Path

We patch `plugin.cc` to check for the `GCS_DEBUG_TRACE_PATH` environment variable. If set, the debug trace file will be written to the specified directory instead of the data directory:

```cpp
// Before (hardcoded):
gcs_module_parameters.add_parameter("communication_debug_path",
                                    mysql_real_data_home);

// After (configurable):
const char *gcs_debug_trace_path_env = getenv("GCS_DEBUG_TRACE_PATH");
if (gcs_debug_trace_path_env != nullptr && gcs_debug_trace_path_env[0] != '\0') {
  gcs_module_parameters.add_parameter("communication_debug_path",
                                      gcs_debug_trace_path_env);
} else {
  gcs_module_parameters.add_parameter("communication_debug_path",
                                      mysql_real_data_home);
}
```

Usage:
```bash
# Set via environment variable
export GCS_DEBUG_TRACE_PATH=/var/log/mysql

# Or via launcher parameter (which sets the env var)
--gcs-debug-trace-path=/var/log/mysql
```

The debug trace file will be written to `/var/log/mysql/GCS_DEBUG_TRACE`.

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

### For XCom SSL verify mode fix:
- `xcom_ssl_verify_fix.patch` - Patch file for `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/network/xcom_network_provider_ssl_native_lib.cc` to enable SSL_VERIFY_PEER when client certificates are configured

### For GCS debug trace path:
- `gcs_debug_trace_path.patch` - Patch file for `plugin/group_replication/src/plugin.cc` to support configurable debug trace path via `GCS_DEBUG_TRACE_PATH` environment variable

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
