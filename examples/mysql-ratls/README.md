# MySQL 8 with RA-TLS Support

This example provides a MySQL 8 server image with transparent RA-TLS (Remote Attestation TLS) support for SGX-based mutual authentication.

## Features

- **Mutual RA-TLS Authentication**: Both client and server must be running in SGX enclaves
- **Certificate-Only Authentication**: MySQL uses X.509 certificates for authentication (no passwords)
- **SGX Quote-Based Certificates**: Certificates are generated at startup from SGX quotes
- **Ethereum Compatible**: Uses secp256k1 curve for certificate generation
- **Smart Contract Whitelist**: Optional whitelist configuration from Ethereum smart contract

## How It Works

1. The container starts with the base `gramine-web3-wallet-docker` image which provides:
   - Intel SGX runtime (aesmd service)
   - PCCS for DCAP attestation
   - RA-TLS libraries (`libratls-quote-verify.so`)

2. The MySQL launcher script:
   - Sets up RA-TLS environment variables (secp256k1 curve, verification enabled)
   - Optionally reads whitelist from smart contract
   - Injects `libratls-quote-verify.so` via `LD_PRELOAD`
   - Starts MySQL with certificate-only authentication

3. When a client connects:
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
| `MYSQL_SSL_DIR` | No | SSL certificate directory (default: `/var/lib/mysql-ssl`) |

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

## License

This example is provided under the same license as the gramine-web3-wallet-docker project.
