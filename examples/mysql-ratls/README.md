# MySQL 8 with RA-TLS Support

This example provides a MySQL 8 server image with transparent RA-TLS (Remote Attestation TLS) support for SGX-based mutual authentication.

## Self-Contained Dockerfile

This example uses a **self-contained Dockerfile** - no external files are needed. All source code (C launcher, Gramine manifest template, build scripts) is embedded directly in the Dockerfile using heredoc syntax. Simply run `docker build` to create the image.

## Features

- **Mutual RA-TLS Authentication**: Both client and server must be running in SGX enclaves
- **Certificate-Only Authentication**: MySQL uses X.509 certificates for authentication (no passwords)
- **SGX Quote-Based Certificates**: Certificates are generated at startup from SGX quotes
- **Ethereum Compatible**: Uses secp256k1 curve for certificate generation
- **Smart Contract Whitelist**: Optional whitelist configuration from Ethereum smart contract
- **Pre-compiled Manifest**: Gramine manifest is pre-compiled and signed during Docker build
- **Encrypted Data Storage**: MySQL data and logs are stored in encrypted partition
- **Group Replication Support**: Multi-node mutual primary-replica mode with automatic IP detection

## How It Works

1. The container starts with the base `gramine-web3-wallet-docker` image which provides:
   - Intel SGX runtime (aesmd service)
   - PCCS for DCAP attestation
   - RA-TLS libraries (`libratls-quote-verify.so`)

2. A C launcher program runs inside the SGX enclave:
   - Reads whitelist configuration from smart contract (if CONTRACT_ADDRESS is set)
   - Sets up RA-TLS environment variables (secp256k1 curve, verification enabled)
   - Uses `execve()` to replace itself with mysqld (avoiding child process overhead in enclave)

3. RA-TLS is injected via `LD_PRELOAD` configured in the Gramine manifest:
   - Transparent TLS interception for MySQL connections
   - Automatic SGX quote generation and verification

4. When a client connects:
   - TLS handshake includes SGX attestation (RA-TLS)
   - Both parties verify each other's SGX quotes
   - If whitelist is configured, client measurements are checked
   - MySQL authenticates using the client's X.509 certificate

## Building

```bash
docker build -t mysql-ratls .
```

## Running

### Basic Usage (No Whitelist)

```bash
docker run -d \
  --name mysql-ratls \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -p 3306:3306 \
  mysql-ratls
```

### With Smart Contract Whitelist

```bash
docker run -d \
  --name mysql-ratls \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e CONTRACT_ADDRESS=0x1234567890abcdef... \
  -e RPC_URL=https://eth-mainnet.example.com \
  -p 3306:3306 \
  mysql-ratls
```

### With Manual Whitelist

```bash
# Create whitelist (Base64-encoded CSV with 5 lines)
# Line 1: MRENCLAVE values (comma-separated hex)
# Line 2: MRSIGNER values (comma-separated hex)
# Line 3: ISV_PROD_ID values
# Line 4: ISV_SVN values
# Line 5: PLATFORM_INSTANCE_ID values
WHITELIST=$(cat <<EOF | base64 -w 0
abc123...,def456...
aaa111...,bbb222...
1,2
1,1
0,0
EOF
)

docker run -d \
  --name mysql-ratls \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e RATLS_WHITELIST_CONFIG=$WHITELIST \
  -p 3306:3306 \
  mysql-ratls
```

## Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `PCCS_API_KEY` | Yes | Intel PCCS API key for DCAP attestation |
| `CONTRACT_ADDRESS` | No | Ethereum contract address to read whitelist from |
| `RPC_URL` | No* | Ethereum RPC endpoint (*required if CONTRACT_ADDRESS is set) |
| `RATLS_WHITELIST_CONFIG` | No | Manual whitelist (Base64-encoded CSV) |
| `MYSQL_DATA_DIR` | No | MySQL data directory (default: `/var/lib/mysql`) |
| `RATLS_CERT_PATH` | No | Path to RA-TLS certificate (default: `/var/lib/mysql-ssl/server-cert.pem`) |
| `RATLS_KEY_PATH` | No | Path to RA-TLS private key, must be in encrypted partition (default: `/app/wallet/mysql-keys/server-key.pem`) |

## Smart Contract Integration

The launcher script reads whitelist configuration from a smart contract that implements:

```solidity
function getSGXConfig() external view returns (string memory);
```

The returned string should be a JSON object containing a `RATLS_WHITELIST_CONFIG` field:

```json
{
  "RATLS_WHITELIST_CONFIG": "BASE64_ENCODED_CSV_WHITELIST",
  "other_config": "..."
}
```

## Whitelist Format

The `RATLS_WHITELIST_CONFIG` is a Base64-encoded CSV with exactly 5 lines:

1. **MRENCLAVE**: Comma-separated hex values of allowed enclave measurements
2. **MRSIGNER**: Comma-separated hex values of allowed signer measurements
3. **ISV_PROD_ID**: Comma-separated product IDs
4. **ISV_SVN**: Comma-separated security version numbers
5. **PLATFORM_INSTANCE_ID**: Comma-separated platform instance IDs

Empty lines or "0" values act as wildcards (allow any value).

## Client Requirements

Clients connecting to this MySQL server must:

1. Run in an SGX enclave with RA-TLS support
2. Use `libratls-quote-verify.so` via LD_PRELOAD
3. Present a valid RA-TLS certificate during TLS handshake
4. Match the whitelist configuration (if enabled)

## Security Considerations

- **No Password Authentication**: This image is configured for certificate-only authentication
- **Mutual Attestation**: Both client and server verify each other's SGX quotes
- **Certificate Regeneration**: Certificates are regenerated on each container start
- **Whitelist Enforcement**: When configured, only clients matching the whitelist can connect

## Troubleshooting

### RA-TLS Library Not Found

If you see "libratls-quote-verify.so not found", ensure the base image is correctly built with Gramine and RA-TLS support.

### Contract Read Failures

If whitelist reading from contract fails:
- Verify `CONTRACT_ADDRESS` is correct
- Verify `RPC_URL` is accessible
- Check that the contract implements `getSGXConfig()`
- Ensure the returned JSON contains `RATLS_WHITELIST_CONFIG` field

### SGX Device Not Available

If SGX devices are not available:
- Ensure the host has SGX enabled in BIOS
- Run container with `--device=/dev/sgx_enclave --device=/dev/sgx_provision`
- Check that SGX drivers are installed on the host

## Group Replication

This image supports MySQL 8 Group Replication for multi-node deployments with mutual primary-replica mode.

### How Group Replication Works

1. **Automatic IP Detection**: The launcher automatically detects:
   - Local LAN IP address (via UDP socket trick)
   - Public IP address (via https://ifconfig.me/ip)

2. **Seed Node Configuration**: Seeds list is built from:
   - Self IPs (LAN + public, deduplicated by ip:port pair)
   - Additional seeds specified via `--gr-seeds` parameter

3. **RA-TLS for Replication**: The `app` user is used for replication with X509 certificate authentication (same RA-TLS certificate as client connections).

### Starting a Group Replication Cluster

#### Bootstrap First Node

```bash
docker run -d \
  --name mysql-gr-node1 \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -p 3306:3306 \
  -p 33061:33061 \
  mysql-ratls \
  --gr-group-name=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee \
  --gr-bootstrap
```

#### Join Additional Nodes

```bash
docker run -d \
  --name mysql-gr-node2 \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -p 3307:3306 \
  -p 33062:33061 \
  mysql-ratls \
  --gr-group-name=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee \
  --gr-seeds=192.168.1.100:33061
```

### Group Replication Parameters

| Parameter | Description |
|-----------|-------------|
| `--gr-group-name=UUID` | Group Replication group name (UUID format). Required to enable GR. |
| `--gr-bootstrap` | Bootstrap a new replication group (first node only). |
| `--gr-seeds=SEEDS` | Comma-separated list of seed nodes (format: `host1:port,host2:port`). Port defaults to 33061 if not specified. |
| `--gr-local-address=ADDR` | Override local address for GR communication (default: auto-detect LAN IP:33061). |

### Group Replication Notes

- **Port 33061**: Used for Group Replication XCom communication. Must be exposed and accessible between nodes.
- **UUID Format**: The `--gr-group-name` must be a valid UUID (e.g., `aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee`).
- **Seed Deduplication**: All seeds (self IPs + extra seeds) are deduplicated by ip:port pair.
- **Certificate Authentication**: Replication uses the same RA-TLS certificate as client connections (X509 required).
- **Multi-Primary Mode**: All nodes can accept writes (mutual primary-replica mode).

## Command-Line Parameters

All environment variables can also be specified via command-line parameters. Parameters take priority over environment variables.

### General Options

| Parameter | Environment Variable | Description |
|-----------|---------------------|-------------|
| `--help`, `-h` | - | Show help message |
| `--contract-address=ADDR` | `CONTRACT_ADDRESS` | Smart contract address for whitelist |
| `--rpc-url=URL` | `RPC_URL` | Ethereum JSON-RPC endpoint URL |
| `--whitelist-config=CFG` | `RATLS_WHITELIST_CONFIG` | Direct whitelist configuration (Base64-encoded CSV) |

### Path Options

| Parameter | Environment Variable | Default | Description |
|-----------|---------------------|---------|-------------|
| `--cert-path=PATH` | `RATLS_CERT_PATH` | `/var/lib/mysql-ssl/server-cert.pem` | Path for RA-TLS certificate |
| `--key-path=PATH` | `RATLS_KEY_PATH` | `/app/wallet/mysql-keys/server-key.pem` | Path for RA-TLS private key |
| `--data-dir=PATH` | `MYSQL_DATA_DIR` | `/app/wallet/mysql-data` | MySQL data directory |

### RA-TLS Configuration Options

| Parameter | Environment Variable | Default | Description |
|-----------|---------------------|---------|-------------|
| `--ra-tls-cert-algorithm=ALG` | `RA_TLS_CERT_ALGORITHM` | - | Certificate algorithm (e.g., secp256r1, secp256k1) |
| `--ratls-enable-verify=0\|1` | `RATLS_ENABLE_VERIFY` | `1` | Enable RA-TLS verification |
| `--ratls-require-peer-cert=0\|1` | `RATLS_REQUIRE_PEER_CERT` | `1` | Require peer certificate for mutual TLS |
| `--ra-tls-allow-outdated-tcb=0\|1` | `RA_TLS_ALLOW_OUTDATED_TCB_INSECURE` | from manifest | Allow outdated TCB (INSECURE) |
| `--ra-tls-allow-hw-config-needed=0\|1` | `RA_TLS_ALLOW_HW_CONFIG_NEEDED` | from manifest | Allow HW configuration needed status |
| `--ra-tls-allow-sw-hardening-needed=0\|1` | `RA_TLS_ALLOW_SW_HARDENING_NEEDED` | from manifest | Allow SW hardening needed status |

### Configuration Validation

The launcher validates configuration and handles mutual exclusions:

- If `--rpc-url` is specified, `--whitelist-config` is ignored (contract whitelist takes precedence)
- Group Replication parameters (`--gr-bootstrap`, `--gr-seeds`, `--gr-local-address`) require `--gr-group-name`
- Warnings are printed when configurations are ignored due to precedence

## License

This example is provided under the same license as the gramine-web3-wallet-docker project.
