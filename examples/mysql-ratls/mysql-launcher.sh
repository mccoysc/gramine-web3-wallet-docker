#!/bin/bash
#
# MySQL RA-TLS Launcher Script
# 
# This script starts MySQL with RA-TLS (Remote Attestation TLS) support.
# It handles:
# 1. Reading whitelist configuration from smart contract (if CONTRACT_ADDRESS is set)
# 2. Setting up RA-TLS environment variables
# 3. Generating RA-TLS certificates using secp256k1 curve (Ethereum compatible)
# 4. Starting MySQL with certificate-only authentication
#
# Environment Variables:
#   CONTRACT_ADDRESS - Smart contract address to read whitelist from (optional)
#   RPC_URL - Ethereum RPC endpoint URL (required if CONTRACT_ADDRESS is set)
#   RATLS_WHITELIST_CONFIG - Manual whitelist override (optional, Base64-encoded CSV)
#   MYSQL_DATA_DIR - MySQL data directory (default: /var/lib/mysql)
#   MYSQL_SSL_DIR - Directory for SSL certificates (default: /var/lib/mysql-ssl)
#   MYSQL_KEY_DIR - Directory for private keys, must be encrypted partition (default: /app/wallet/mysql-keys)
#

set -e

echo "=========================================="
echo "MySQL RA-TLS Launcher"
echo "=========================================="
echo ""

# Default paths
MYSQL_DATA_DIR="${MYSQL_DATA_DIR:-/var/lib/mysql}"
MYSQL_SSL_DIR="${MYSQL_SSL_DIR:-/var/lib/mysql-ssl}"
# Private keys must be stored in encrypted partition for security
# /app/wallet is the Gramine encrypted filesystem mount point
MYSQL_KEY_DIR="${MYSQL_KEY_DIR:-/app/wallet/mysql-keys}"

# Create directories if they don't exist
# SSL directory for certificates (can be in normal filesystem)
mkdir -p "$MYSQL_SSL_DIR"
# Key directory for private keys (must be in encrypted partition)
mkdir -p "$MYSQL_KEY_DIR"

# RA-TLS Certificate Configuration
# Use secp256k1 curve for Ethereum compatibility
export RA_TLS_CERT_ALGORITHM="secp256k1"

# Enable RA-TLS verification and require peer certificate for mutual TLS
export RATLS_ENABLE_VERIFY=1
export RATLS_REQUIRE_PEER_CERT=1

# Set certificate and key paths for RA-TLS
# Certificate can be in normal directory
export RATLS_CERT_PATH="${MYSQL_SSL_DIR}/server-cert.pem"
# Private key MUST be in encrypted partition for security
export RATLS_KEY_PATH="${MYSQL_KEY_DIR}/server-key.pem"

echo "RA-TLS Configuration:"
echo "  Certificate Algorithm: secp256k1 (Ethereum compatible)"
echo "  Certificate Path: $RATLS_CERT_PATH (normal directory)"
echo "  Key Path: $RATLS_KEY_PATH (encrypted partition)"
echo "  Verification Enabled: $RATLS_ENABLE_VERIFY"
echo "  Require Peer Certificate: $RATLS_REQUIRE_PEER_CERT"
echo ""

# Function to read SGX config from contract and extract RATLS_WHITELIST_CONFIG
read_whitelist_from_contract() {
    local contract_address="$1"
    local rpc_url="$2"
    
    echo "Reading whitelist from contract..."
    echo "  Contract Address: $contract_address"
    echo "  RPC URL: $rpc_url"
    
    # Use Node.js to interact with the contract
    # This script reads getSGXConfig() and extracts RATLS_WHITELIST_CONFIG field
    local result
    result=$(node -e "
const { ethers } = require('ethers');

async function main() {
    try {
        const provider = new ethers.JsonRpcProvider('${rpc_url}');
        
        // Minimal ABI for getSGXConfig
        const abi = ['function getSGXConfig() view returns (string)'];
        const contract = new ethers.Contract('${contract_address}', abi, provider);
        
        // Call getSGXConfig
        const sgxConfig = await contract.getSGXConfig();
        
        if (!sgxConfig || sgxConfig === '') {
            console.error('SGX config is empty');
            process.exit(0);
        }
        
        // Try to parse as JSON
        let config;
        try {
            config = JSON.parse(sgxConfig);
        } catch (e) {
            console.error('Failed to parse SGX config as JSON:', e.message);
            process.exit(0);
        }
        
        // Extract RATLS_WHITELIST_CONFIG field
        if (config.RATLS_WHITELIST_CONFIG) {
            console.log(config.RATLS_WHITELIST_CONFIG);
        } else {
            console.error('RATLS_WHITELIST_CONFIG field not found in SGX config');
            process.exit(0);
        }
    } catch (e) {
        console.error('Error reading from contract:', e.message);
        process.exit(1);
    }
}

main();
" 2>&1)
    
    local exit_code=$?
    
    if [ $exit_code -ne 0 ]; then
        echo "  Warning: Failed to read from contract"
        echo "  Error: $result"
        return 1
    fi
    
    # Check if result contains error messages (stderr)
    if echo "$result" | grep -q "^Error\|^Failed\|^SGX config is empty\|^RATLS_WHITELIST_CONFIG field not found"; then
        echo "  Warning: $result"
        return 1
    fi
    
    # Return the whitelist config
    echo "$result"
    return 0
}

# Handle whitelist configuration
if [ -n "$CONTRACT_ADDRESS" ]; then
    echo "Contract address specified: $CONTRACT_ADDRESS"
    
    if [ -z "$RPC_URL" ]; then
        echo "Warning: RPC_URL not set, cannot read from contract"
        echo "  Falling back to environment-based whitelist (if set)"
    else
        # Try to read whitelist from contract
        whitelist_from_contract=$(read_whitelist_from_contract "$CONTRACT_ADDRESS" "$RPC_URL" 2>&1 | tail -1)
        
        if [ -n "$whitelist_from_contract" ] && ! echo "$whitelist_from_contract" | grep -q "^Warning\|^Error"; then
            echo "Successfully read whitelist from contract"
            export RATLS_WHITELIST_CONFIG="$whitelist_from_contract"
        else
            echo "Could not read valid whitelist from contract"
            echo "  Using environment-based whitelist (if set)"
        fi
    fi
else
    echo "No CONTRACT_ADDRESS specified"
    echo "  Using environment-based whitelist (if set)"
fi

# Display whitelist status
if [ -n "$RATLS_WHITELIST_CONFIG" ]; then
    echo ""
    echo "Whitelist Configuration:"
    echo "  RATLS_WHITELIST_CONFIG is set"
    echo "  Only clients matching the whitelist can connect"
    
    # Decode and display whitelist info (for debugging)
    decoded=$(echo "$RATLS_WHITELIST_CONFIG" | base64 -d 2>/dev/null || echo "")
    if [ -n "$decoded" ]; then
        line_count=$(echo "$decoded" | wc -l)
        echo "  Whitelist contains $line_count measurement lines"
    fi
else
    echo ""
    echo "Whitelist Configuration:"
    echo "  No whitelist configured"
    echo "  Any valid RA-TLS client can connect"
fi

echo ""
echo "=========================================="
echo "Starting MySQL Server"
echo "=========================================="
echo ""

# Find the RA-TLS library path
RATLS_LIB_PATH=$(ldconfig -p | grep 'libratls-quote-verify.so ' | awk '{print $NF}' | head -1)

if [ -z "$RATLS_LIB_PATH" ]; then
    # Try common paths
    for path in /usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so \
                /usr/local/lib/libratls-quote-verify.so \
                /usr/lib/x86_64-linux-gnu/libratls-quote-verify.so; do
        if [ -f "$path" ]; then
            RATLS_LIB_PATH="$path"
            break
        fi
    done
fi

if [ -n "$RATLS_LIB_PATH" ]; then
    echo "RA-TLS library found: $RATLS_LIB_PATH"
    export LD_PRELOAD="$RATLS_LIB_PATH"
else
    echo "Warning: libratls-quote-verify.so not found"
    echo "  RA-TLS injection will not be available"
fi

# MySQL configuration for certificate-only authentication
# These will be passed to mysqld
# Note: Certificate is in normal directory, but private key is in encrypted partition
MYSQL_SSL_ARGS=(
    "--ssl-ca=${MYSQL_SSL_DIR}/ca.pem"
    "--ssl-cert=${MYSQL_SSL_DIR}/server-cert.pem"
    "--ssl-key=${MYSQL_KEY_DIR}/server-key.pem"
    "--require-secure-transport=ON"
)

# Check if this is the first run (need to initialize MySQL)
if [ ! -d "$MYSQL_DATA_DIR/mysql" ]; then
    echo "Initializing MySQL data directory..."
    mysqld --initialize-insecure --user=mysql --datadir="$MYSQL_DATA_DIR"
    echo "MySQL initialized"
fi

echo ""
echo "Starting mysqld with RA-TLS..."
echo "  Data directory: $MYSQL_DATA_DIR"
echo "  SSL directory: $MYSQL_SSL_DIR"
echo ""

# Use exec to replace this process with mysqld
# This ensures proper signal handling and that mysqld is PID 1
exec mysqld \
    --user=mysql \
    --datadir="$MYSQL_DATA_DIR" \
    "${MYSQL_SSL_ARGS[@]}" \
    "$@"
