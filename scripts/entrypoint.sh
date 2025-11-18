#!/bin/bash

set -e

echo "=========================================="
echo "Gramine Web3 Wallet Container"
echo "=========================================="
echo ""

echo "Starting aesmd service..."
mkdir -p /var/run/aesmd
/usr/sbin/aesmd &
AESMD_PID=$!

echo "Waiting for aesmd to be ready..."
TIMEOUT=30
ELAPSED=0
while [ ! -S /var/run/aesmd/aesm.socket ]; do
    if [ $ELAPSED -ge $TIMEOUT ]; then
        echo "⚠ Warning: aesmd socket not ready after ${TIMEOUT}s"
        echo "  This may affect SGX functionality"
        break
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

if [ -S /var/run/aesmd/aesm.socket ]; then
    echo "✓ aesmd service started successfully"
else
    echo "⚠ aesmd service may not be running properly"
fi

echo ""

if [ -e /dev/sgx_enclave ]; then
    echo "✓ SGX device detected: /dev/sgx_enclave"
    if [ -e /dev/sgx_provision ]; then
        echo "✓ SGX provision device detected: /dev/sgx_provision"
    else
        echo "⚠ SGX provision device not found: /dev/sgx_provision"
        echo "  DCAP attestation may not work properly"
    fi
    export GRAMINE_SGX_MODE=1
    export GRAMINE_DIRECT_MODE=0
else
    echo "⚠ SGX device not found, running in direct mode"
    echo "  To enable SGX, run container with:"
    echo "  --device=/dev/sgx_enclave --device=/dev/sgx_provision"
    export GRAMINE_SGX_MODE=0
    export GRAMINE_DIRECT_MODE=1
fi

echo ""
echo "=========================================="
echo "Installed Software Versions"
echo "=========================================="

echo ""
echo "Python version:"
python3 --version

echo ""
echo "Node.js version:"
node --version
npm --version

echo ""
echo "Gramine version:"
if [ "$GRAMINE_SGX_MODE" = "1" ]; then
    gramine-sgx --version 2>/dev/null || echo "gramine-sgx not found"
else
    gramine-direct --version 2>/dev/null || echo "gramine-direct not found"
fi

echo ""
echo "Available Web3 tools:"
which web3 >/dev/null 2>&1 && echo "  ✓ web3" || echo "  - web3 (not installed)"
which hardhat >/dev/null 2>&1 && echo "  ✓ hardhat" || echo "  - hardhat (not installed)"
which truffle >/dev/null 2>&1 && echo "  ✓ truffle" || echo "  - truffle (not installed)"
which ganache >/dev/null 2>&1 && echo "  ✓ ganache" || echo "  - ganache (not installed)"

echo ""
echo "=========================================="
echo "Container ready!"
echo "=========================================="
echo ""

exec "$@"
