# MySQL Client with RA-TLS Support

This example provides a MySQL client image with transparent RA-TLS (Remote Attestation TLS) support for SGX-based mutual authentication. It is designed to connect to the `mysql-ratls` server example.

## Self-Contained Dockerfile

This example uses a **self-contained Dockerfile** - no external files are needed. All source code (C launcher, Node.js client script, Gramine manifest template, build scripts) is downloaded from the repository during the Docker build. Simply run `docker build` to create the image.

## Features

- **Mutual RA-TLS Authentication**: Both client and server must be running in SGX enclaves
- **Certificate-Only Authentication**: MySQL uses X.509 certificates for authentication (no passwords)
- **SGX Quote-Based Certificates**: Certificates are generated at startup from SGX quotes
- **Ethereum Compatible**: Uses secp256k1 curve for certificate generation
- **Smart Contract Whitelist**: Optional whitelist configuration from Ethereum smart contract
- **Pre-compiled Manifest**: Gramine manifest is pre-compiled and signed during Docker build
- **Interactive Mode**: Supports interactive SQL queries

## How It Works

1. The container starts with the base `gramine-web3-wallet-docker` image which provides:
   - Intel SGX runtime (aesmd service)
   - PCCS for DCAP attestation
   - RA-TLS libraries (`libratls-quote-verify.so`)
   - Node.js runtime

2. A C launcher program runs inside the SGX enclave:
   - Reads whitelist configuration from smart contract (if CONTRACT_ADDRESS is set)
   - Sets up RA-TLS environment variables
   - Sets LD_PRELOAD for RA-TLS injection
   - Uses `execve()` to replace itself with Node.js (avoiding child process overhead in enclave)

3. Node.js runs the MySQL client script with RA-TLS injected via LD_PRELOAD:
   - Transparent TLS interception for MySQL connections
   - Automatic SGX quote generation and verification
   - Certificate-based authentication with the MySQL server

4. When connecting to the server:
   - TLS handshake includes SGX attestation (RA-TLS)
   - Both parties verify each other's SGX quotes
   - If whitelist is configured, server measurements are checked
   - MySQL authenticates using the client's X.509 certificate

## Building

```bash
docker build -t mysql-client-ratls .
```

## Running

### Basic Usage (Connect to MySQL Server)

```bash
docker run -it \
  --name mysql-client-ratls \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e MYSQL_HOST=mysql-server-hostname \
  -e MYSQL_USER=app \
  -e MYSQL_INTERACTIVE=1 \
  mysql-client-ratls
```

### Execute Single Query

```bash
docker run --rm \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e MYSQL_HOST=mysql-server-hostname \
  -e MYSQL_USER=app \
  -e MYSQL_DATABASE=mydb \
  -e MYSQL_QUERY="SELECT * FROM users LIMIT 10" \
  mysql-client-ratls
```

### With Smart Contract Whitelist

```bash
docker run -it \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e CONTRACT_ADDRESS=0x1234567890abcdef... \
  -e RPC_URL=https://eth-mainnet.example.com \
  -e MYSQL_HOST=mysql-server-hostname \
  -e MYSQL_USER=app \
  -e MYSQL_INTERACTIVE=1 \
  mysql-client-ratls
```

### Connect to mysql-ratls Server (Docker Network)

```bash
# Create a network
docker network create sgx-network

# Start the mysql-ratls server
docker run -d \
  --name mysql-server \
  --network sgx-network \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  mysql-ratls

# Connect with the client
docker run -it \
  --name mysql-client \
  --network sgx-network \
  --device=/dev/sgx_enclave \
  --device=/dev/sgx_provision \
  -e PCCS_API_KEY=your_intel_api_key \
  -e MYSQL_HOST=mysql-server \
  -e MYSQL_USER=app \
  -e MYSQL_INTERACTIVE=1 \
  mysql-client-ratls
```

## Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `PCCS_API_KEY` | Yes | Intel PCCS API key for DCAP attestation |
| `MYSQL_HOST` | No | MySQL server hostname (default: `localhost`) |
| `MYSQL_PORT` | No | MySQL server port (default: `3306`) |
| `MYSQL_USER` | No | MySQL username (default: `app`) |
| `MYSQL_DATABASE` | No | MySQL database to connect to |
| `MYSQL_QUERY` | No | SQL query to execute (exits after execution) |
| `MYSQL_INTERACTIVE` | No | Set to `1` for interactive mode |
| `CONTRACT_ADDRESS` | No | Ethereum contract address to read whitelist from |
| `RPC_URL` | No* | Ethereum RPC endpoint (*required if CONTRACT_ADDRESS is set) |

## RA-TLS Certificate Configuration

The following RA-TLS environment variables are configured by the C launcher (following [mccoysc/gramine tools/sgx/ra-tls/CERTIFICATE_CONFIGURATION.md](https://github.com/mccoysc/gramine/blob/main/tools/sgx/ra-tls/CERTIFICATE_CONFIGURATION.md)):

| Variable | Value | Description |
|----------|-------|-------------|
| `RA_TLS_CERT_ALGORITHM` | `secp256k1` | Elliptic curve for certificate generation (Ethereum compatible) |
| `RA_TLS_ENABLE_VERIFY` | `1` | Enable RA-TLS verification during TLS handshake |
| `RA_TLS_REQUIRE_PEER_CERT` | `1` | Require peer certificate for mutual TLS authentication |
| `RA_TLS_CERT_PATH` | `/var/lib/mysql-client-ssl/client-cert.pem` | Path to store generated certificate (regular directory) |
| `RA_TLS_KEY_PATH` | `/app/wallet/mysql-client-keys/client-key.pem` | Path to store private key (encrypted partition) |

These values are hardcoded in the manifest and launcher for security (not passthrough from host environment). The private key is stored in the encrypted partition (`/app/wallet/`) to prevent data leakage.

## Smart Contract Integration

The launcher can read whitelist configuration from a smart contract that implements:

```solidity
function getSGXConfig() external view returns (string memory);
```

The returned string should be a JSON object containing a `RA_TLS_WHITELIST_CONFIG` field:

```json
{
  "RA_TLS_WHITELIST_CONFIG": "BASE64_ENCODED_CSV_WHITELIST",
  "other_config": "..."
}
```

## Whitelist Format

The `RA_TLS_WHITELIST_CONFIG` is a Base64-encoded CSV with exactly 5 lines:

1. **MRENCLAVE**: Comma-separated hex values of allowed enclave measurements
2. **MRSIGNER**: Comma-separated hex values of allowed signer measurements
3. **ISV_PROD_ID**: Comma-separated product IDs
4. **ISV_SVN**: Comma-separated security version numbers
5. **PLATFORM_INSTANCE_ID**: Comma-separated platform instance IDs

Empty lines or "0" values act as wildcards (allow any value).

## Server Requirements

The MySQL server this client connects to must:

1. Run in an SGX enclave with RA-TLS support (e.g., `mysql-ratls` example)
2. Use `libratls-quote-verify.so` via LD_PRELOAD
3. Present a valid RA-TLS certificate during TLS handshake
4. Be configured for certificate-only authentication (REQUIRE X509)

## Security Considerations

- **No Password Authentication**: This client is configured for certificate-only authentication
- **Mutual Attestation**: Both client and server verify each other's SGX quotes
- **Certificate Regeneration**: Certificates are regenerated on each container start
- **Private Key Protection**: Private keys are stored in encrypted partition (`/app/wallet/`)
- **Whitelist Enforcement**: When configured, only servers matching the whitelist can be connected to

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    SGX Enclave                               │
│  ┌─────────────────┐      ┌─────────────────────────────┐   │
│  │  C Launcher     │      │  Node.js + mysql-client.js  │   │
│  │                 │      │                             │   │
│  │  1. Read config │      │  3. Connect to MySQL        │   │
│  │  2. Set LD_PRELOAD ──► │  4. RA-TLS handshake       │   │
│  │     execve()    │      │  5. Execute queries         │   │
│  └─────────────────┘      └─────────────────────────────┘   │
│                                      │                       │
│                           ┌──────────▼──────────┐           │
│                           │ libratls-quote-     │           │
│                           │ verify.so           │           │
│                           │ (LD_PRELOAD)        │           │
│                           └─────────────────────┘           │
└─────────────────────────────────────────────────────────────┘
                               │
                               │ TLS + SGX Quote
                               ▼
                    ┌─────────────────────┐
                    │  MySQL Server       │
                    │  (mysql-ratls)      │
                    │  in SGX Enclave     │
                    └─────────────────────┘
```

## Troubleshooting

### RA-TLS Library Not Found

If you see "libratls-quote-verify.so not found", ensure the base image is correctly built with Gramine and RA-TLS support.

### Connection Refused

- Verify the MySQL server is running and accessible
- Check that `MYSQL_HOST` is correctly set
- Ensure both containers are on the same Docker network

### Certificate Verification Failed

- Both client and server must be running in SGX enclaves
- Verify PCCS is properly configured with a valid API key
- Check that whitelist configuration (if used) includes the server's measurements

### SGX Device Not Available

If SGX devices are not available:
- Ensure the host has SGX enabled in BIOS
- Run container with `--device=/dev/sgx_enclave --device=/dev/sgx_provision`
- Check that SGX drivers are installed on the host

## License

This example is provided under the same license as the gramine-web3-wallet-docker project.
