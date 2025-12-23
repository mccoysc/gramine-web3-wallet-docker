# MySQL Group Replication Patch for SGX/Gramine

This directory contains modified MySQL Group Replication plugin source files that enable GR to work in SGX/Gramine environments. The patches address nine main issues:

1. **Network Interface Enumeration**: The standard `getifaddrs()` system call is not available in Gramine/SGX due to netlink socket limitations.
2. **SSL CA Configuration**: MySQL unconditionally passes an empty `ca_file` parameter to XCom SSL initialization, causing failures when using self-signed certificates without a CA (e.g., RA-TLS).
3. **GCS Debug Trace Path**: The GCS_DEBUG_TRACE file is hardcoded to be written in the MySQL data directory, which is in an encrypted partition in SGX environments and cannot be read from outside.
4. **XCom SSL Verify Mode**: When `group_replication_ssl_mode=REQUIRED`, MySQL sets `SSL_VERIFY_NONE` for both server and client SSL contexts, which prevents mutual TLS certificate exchange required for RA-TLS.
5. **SSL Accept Error Logging**: When SSL_accept fails, MySQL only logs a generic error message without detailed OpenSSL error information, making it difficult to diagnose SSL handshake failures.
6. **XCom Node Detection Race Condition**: When a new node joins the cluster, its `detected` timestamp is initialized to 0, causing it to be immediately marked as "not alive" before any message exchange can occur. This race condition is exacerbated in SGX/RA-TLS environments where SSL handshakes are slower.
7. **XCom Connection Timeout Too Short**: The `dial()` function in XCom uses a hardcoded 1-second (1000ms) connection timeout for both TCP connect and SSL handshake. This is insufficient for RA-TLS in SGX environments where SGX quote generation and verification can take several seconds.
8. **XCom Detector Live Timeout Too Short**: The `DETECTOR_LIVE_TIMEOUT` constant is hardcoded to 5 seconds. In SGX/RA-TLS environments, the time from connection establishment to first XCom message exchange can exceed 5 seconds due to slow SSL handshakes and protocol negotiation, causing the detector to incorrectly mark nodes as dead.
9. **View Change Notification Debug Logging**: When Node2 joins Node1's cluster, the "Timeout on wait for view after joining group" error occurs after ~35 seconds. This debug patch adds comprehensive logging to trace the view change notification flow and identify the root cause.

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

### For SSL accept error logging:
- `ssl_accept_error_logging.patch` - Patch file for `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/network/xcom_network_provider.cc` to add detailed OpenSSL error logging when SSL_accept fails

### For XCom node detection race condition fix:
- `xcom_dial_server_detected.patch` - Patch file for `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_transport.cc` to initialize `detected` timestamp to current time in both `mksrv()` function (new server creation) and `update_servers()` function (server object reuse)

### For XCom connection timeout fix:
- `xcom_dial_connection_timeout.patch` - Patch file for `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_transport.cc` to increase the connection timeout from 1000ms to 30000ms in the `dial()` function

### For XCom detector live timeout fix:
- `xcom_detector_live_timeout.patch` - Patch file for `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_detector.h` to increase the `DETECTOR_LIVE_TIMEOUT` from 5.0 to 30.0 seconds

## Problem 5: SSL Accept Error Logging

When SSL_accept fails during XCom SSL handshake, MySQL only logs a generic "acceptor learner accept SSL failed" message without any details about why the handshake failed. This makes it extremely difficult to diagnose SSL issues, especially in RA-TLS scenarios where certificate verification involves SGX quote validation.

## Solution 5: Detailed OpenSSL Error Logging

We patch `xcom_network_provider.cc` to log detailed error information when SSL_accept fails:

1. **SSL_get_error code**: The specific SSL error type (e.g., SSL_ERROR_SSL, SSL_ERROR_SYSCALL)
2. **SSL_get_verify_result**: The certificate verification result code
3. **Peer certificate presence**: Whether the peer sent a certificate
4. **OpenSSL error stack**: All errors from the OpenSSL error queue with human-readable descriptions

This information appears in the GCS_DEBUG_TRACE file and helps diagnose issues like:
- Client not sending certificate
- Certificate verification failures
- Signature algorithm mismatches
- RA-TLS quote verification failures

## Problem 6: XCom Node Detection Race Condition

When a new node joins the MySQL Group Replication cluster, XCom's detector mechanism may incorrectly mark it as "not alive" before any message exchange can occur. This happens because:

1. **`task_now()` returns epoch seconds** (~1.7 billion), not relative time
2. **New server's `detected` timestamp is initialized to 0.0** in `mksrv()` (xcom_transport.cc:600)
3. **The DETECT macro** checks `detected[i] + 5.0 > task_now()`, which is always FALSE when `detected=0.0`
4. **`server_detected()` is only called when receiving messages** via `dispatch_op()`, not when establishing connections
5. **`alive()` only updates `s->active`**, not `s->detected`

In normal environments, the first message exchange happens very quickly (milliseconds), so `note_detected()` is called before the detector checks the node status. However, in SGX/RA-TLS environments:
- SSL handshakes are slower due to SGX enclave overhead and RA-TLS quote generation/verification
- The time window between connection establishment and first message exchange is longer
- The detector may run and mark the new node as "failed" before any message arrives

This causes the new node to be classified as a "non-member suspect", leading to connection closure and "Broken pipe" errors on the joining node's SSL_accept.

## Solution 6: Initialize detected Timestamp in mksrv() and update_servers()

We patch `xcom_transport.cc` in two locations to ensure the `detected` timestamp is always set to `task_now()`:

### Part 1: New server creation in mksrv()

```cpp
// Before (initialized to 0.0):
s->active = 0.0;
s->detected = 0.0;

// After (initialized to current time):
s->active = 0.0;
s->detected = task_now();  // Give new server a 5-second grace period
```

### Part 2: Site detected array initialization in update_servers()

There are TWO separate `detected` fields in XCom:
- `server->detected`: Timestamp in the server object (set by `mksrv()` fix above)
- `site->detected[i]`: Timestamp array in the site_def structure (used by `DETECT()` macro)

The `DETECT(site, i)` macro checks `site->detected[i]`, NOT `server->detected`. The `update_detected()` function synchronizes these values, but it runs in the `detector_task` loop (every 1 second). When a new node joins, the liveness check may run BEFORE `update_detected()` has synchronized the values.

We add a fix to initialize BOTH `server->detected` AND `site->detected[i]` immediately after `s->servers[i]` is assigned, covering BOTH the reuse path ("Using existing server node") AND the new creation path ("Creating new server node"):

```cpp
// Before (detected fields not initialized):
if (sp) {
  s->servers[i] = sp;
  ...
} else {
  s->servers[i] = addsrv(name, port);
}
IFDBG(D_BUG, FN; PTREXP(s->servers[i]));

// After (BOTH detected fields initialized for BOTH paths):
if (sp) {
  s->servers[i] = sp;
  ...
} else {
  s->servers[i] = addsrv(name, port);
}
/* Initialize detected timestamp for both new and reused servers */
s->servers[i]->detected = task_now();
s->detected[i] = task_now();
IFDBG(D_BUG, FN; PTREXP(s->servers[i]));
```

**Why BOTH fields must be updated:**
- `site->detected[i]` is what `DETECT()` macro checks for liveness
- `server->detected` is synchronized to `site->detected[i]` by `update_detected()` every second
- If only `site->detected[i]` is set, `update_detected()` will overwrite it with the old `server->detected` value on the next detector cycle
- Setting both fields ensures the fix persists across detector cycles

**Why this location (after if/else block):**
- Covers BOTH the reuse path (`if (sp)`) AND the new creation path (`else { addsrv() }`)
- Placing the fix after the if/else ensures both fields are always initialized regardless of which path is taken

This fix:
- Gives both newly created and reused server objects a 5-second grace period (DETECTOR_LIVE_TIMEOUT) before being marked as "not alive"
- Allows time for `dial()` to complete and first message exchange to occur in slower SGX/RA-TLS environments
- Does not mask real failures: if no messages are received within 5 seconds, the node will still be marked as failed
- Affects all server objects assigned to site_def, ensuring consistent behavior

## Problem 7: XCom Connection Timeout Too Short

The `dial()` function in XCom (xcom_transport.cc) uses a hardcoded 1-second (1000ms) timeout for `open_new_connection()`. This timeout is used for both TCP connect and SSL handshake operations. In SGX/RA-TLS environments, this timeout is insufficient because:

1. **SGX Quote Generation**: When establishing an SSL connection, the RA-TLS library needs to generate an SGX quote, which involves communication with the AESM service and can take several seconds
2. **SGX Quote Verification**: The peer needs to verify the SGX quote, which may involve fetching attestation collateral from PCCS/Intel PCS
3. **Network Latency**: Cross-datacenter deployments may have higher network latency

When the timeout expires during SSL handshake, the connection is closed, resulting in:
- Node1 logs: `Connecting socket to address X.X.X.X in port 33062 failed with error 0-Success.`
- Node2 logs: `acceptor learner accept SSL failed, SSL_get_error=5` and `OpenSSL error: error:80000020:system library::Broken pipe`

The "error 0-Success" message is misleading because `poll_for_timed_connects()` clears errno when the timeout expires.

## Solution 7: Increase Connection Timeout to 30 Seconds

We patch `xcom_transport.cc` to increase the connection timeout from 1000ms to 30000ms (30 seconds):

```cpp
// Before (1 second timeout):
s->con = open_new_connection(s->srv, s->port, 1000, dial_call_log_level);

// After (30 second timeout for RA-TLS):
/* Increased timeout from 1000ms to 30000ms for RA-TLS in SGX environments */
s->con = open_new_connection(s->srv, s->port, 30000, dial_call_log_level);
```

This fix:
- Provides sufficient time for SGX quote generation and verification during SSL handshake
- Allows cross-datacenter deployments with higher network latency to succeed
- Does not affect normal operation: connections that complete quickly will still complete quickly
- The 30-second timeout is a reasonable upper bound that balances reliability with failure detection

## Problem 8: XCom Detector Live Timeout Too Short

The `DETECTOR_LIVE_TIMEOUT` constant in `xcom_detector.h` is hardcoded to 5.0 seconds. This timeout determines how long XCom waits for messages from a peer before marking it as "not alive". In SGX/RA-TLS environments, this timeout is insufficient because:

1. **Bidirectional Connection Establishment**: When Node2 joins the cluster, it connects to Node1. Node1 then makes a "reverse connection" back to Node2. This reverse connection's SSL handshake can take ~5 seconds due to RA-TLS quote generation/verification.

2. **Message Exchange Delay**: After the reverse connection is established, XCom needs to send protocol negotiation messages. The `sender_task` may not immediately send messages after connection establishment, as it waits for certain conditions (e.g., `wakeup_sender()` to be called).

3. **Join Request Response Delay**: Node2 sends a join request to Node1, but Node1 cannot respond until the reverse connection is fully established. If this takes more than 5 seconds, Node2's detector marks Node1 as dead.

The typical failure sequence is:
1. Node2 connects to Node1 and sends join request (T=0)
2. Node1 starts reverse connection to Node2 (T=0)
3. Reverse connection SSL handshake completes (T=5s)
4. Node2's detector marks Node1 as dead (no response within 5s) (T=5s)
5. Node2 closes the connection
6. Node1's reverse connection fails with "Failure reading from fd=-1" (T=10s)

## Solution 8: Increase Detector Live Timeout to 30 Seconds

We patch `xcom_detector.h` to increase the `DETECTOR_LIVE_TIMEOUT` from 5.0 to 30.0 seconds:

```cpp
// Before (5 second timeout):
#define DETECTOR_LIVE_TIMEOUT 5.0

// After (30 second timeout for RA-TLS):
/* Increased from 5.0 to 30.0 for RA-TLS in SGX environments */
#define DETECTOR_LIVE_TIMEOUT 30.0
```

Additionally, we update `xcom_dial_server_detected.patch` to call `server_detected(s)` when a connection is successfully established in `dial()`, ensuring the detected timestamp is refreshed when the connection completes (not just when messages are received):

```cpp
// Added after set_connected(s->con, CON_FD):
/* Update detected timestamp when connection is established */
server_detected(s);
```

This combined fix:
- Provides a 30-second grace period for RA-TLS handshakes and protocol negotiation
- Updates the detected timestamp when connections are established, not just when messages arrive
- Allows sufficient time for bidirectional connection establishment in SGX environments
- Trade-off: Real node failures will take up to 30 seconds to detect (slower failover)

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

## Problem 9: View Change Notification Debug Logging

When Node2 attempts to join Node1's cluster, the join fails with "Timeout on wait for view after joining group" error after approximately 35 seconds, even though the `VIEW_MODIFICATION_TIMEOUT` is set to 60 seconds. Analysis of the GCS_DEBUG_TRACE logs shows:

1. Node1 successfully connects to Node2:33062 three times (SSL handshakes succeed)
2. Node1 sees `process_view: total_number_nodes=2` (Node2 joined successfully at XCom layer)
3. Then connections start failing with "error 0-Success" (poll timeout)
4. Node2's XCom port 33062 closes prematurely

The root cause is unclear because the XCom layer successfully delivers the view to both nodes, but the GR plugin layer on Node2 may not be processing the view change notification correctly. Possible causes include:

1. **Clock skew in SGX/Gramine**: The `wait_for_view_modification()` function uses `CLOCK_REALTIME` for the timeout deadline. In SGX enclaves, `CLOCK_REALTIME` can be unstable or drift, causing a 60-second timeout to expire in ~35 seconds of wall time.

2. **Notifier mismatch**: The view change event handler might be using a different notifier instance than the one being waited on in `initialize_plugin_and_join()`.

3. **View event not reaching GR plugin layer**: The XCom layer delivered the view, but the GR plugin layer on Node2 might not have processed it due to thread blockage or an error in the event handler chain.

4. **`mysql_cond_timedwait` returning unexpected error**: The function treats any non-zero return as a timeout, but it could be returning an error code other than `ETIMEDOUT`.

## Solution 9: Comprehensive Debug Logging

We add detailed logging to trace the view change notification flow:

### 1. `gcs_view_modification_notifier.cc` - Notifier state machine

Logs are added to `start_view_modification()`, `end_view_modification()`, `cancel_view_modification()`, and `wait_for_view_modification()` with:
- Notifier pointer address (to detect notifier mismatch)
- Thread ID (to detect thread blockage)
- Both `CLOCK_REALTIME` and `CLOCK_MONOTONIC` timestamps (to detect clock skew)
- `mysql_cond_timedwait` return code and whether it equals `ETIMEDOUT`
- Computed deadline and time until deadline
- Elapsed time (both realtime and monotonic)

### 2. `gcs_operations.cc` - Notifier registration/unregistration

Logs are added to `notify_of_view_change_end()`, `notify_of_view_change_cancellation()`, and `remove_view_notifer()` with:
- Notifier list size before/after operations
- Notifier pointer addresses being iterated

### 3. `gcs_event_handlers.cc` - View change handler

Logs are added to `on_view_changed()` and `handle_joining_members()` with:
- `is_joining` and `is_leaving` flags
- View member counts
- `check_group_compatibility()` result
- Which notification path is taken (end vs cancel)

### 4. `plugin.cc` - Join initialization

Logs are added around `wait_for_view_modification()` call with:
- Notifier pointer address
- Whether wait returned due to timeout or cancellation

### Log Format

All logs use the format `[COMPONENT] function: key=value ...` and include:
- `realtime=X.XXXXXX` - `CLOCK_REALTIME` timestamp
- `monotonic=X.XXXXXX` - `CLOCK_MONOTONIC` timestamp
- `thread=XXXXXXXX` - Thread ID

### How to Use

1. Apply the patch and rebuild the GR plugin
2. Start Node1 and Node2 with GR enabled
3. Capture stderr output from both nodes
4. Look for the following patterns:
   - `[VIEW-NOTIFIER] wait_for_view_modification BEGIN` - When wait starts
   - `[VIEW-NOTIFIER] wait_for_view_modification AFTER_WAIT` - After each timedwait
   - `[GCS-EVENT] handle_joining_members` - When view change is processed
   - `[VIEW-NOTIFIER] end_view_modification` - When notifier is signaled
   - `[VIEW-NOTIFIER] wait_for_view_modification END` - When wait completes

### Diagnosing the Issue

Compare the timestamps to identify:
- **Clock skew**: If `elapsed_realtime` differs significantly from `elapsed_monotonic`, clock skew is present
- **Notifier mismatch**: If the notifier pointer in `wait_for_view_modification` differs from the one in `end_view_modification`
- **Handler not called**: If `handle_joining_members` is never logged on Node2
- **Unexpected error**: If `result != 0` but `is_ETIMEDOUT=0`, the timedwait returned an unexpected error

### Files Modified

- `view_change_debug_logging.patch` - Patch file for:
  - `plugin/group_replication/src/gcs_view_modification_notifier.cc`
  - `plugin/group_replication/src/gcs_operations.cc`
  - `plugin/group_replication/src/gcs_event_handlers.cc`
  - `plugin/group_replication/src/plugin.cc`
