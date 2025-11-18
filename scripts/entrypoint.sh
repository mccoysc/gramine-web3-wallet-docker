#!/bin/bash

set -e

echo "=========================================="
echo "Gramine Web3 Wallet Container"
echo "=========================================="
echo ""

if [ -e /dev/sgx_enclave ]; then
    echo "✓ SGX device detected: /dev/sgx_enclave"
    export GRAMINE_SGX_MODE=1
    export GRAMINE_DIRECT_MODE=0
else
    echo "⚠ SGX device not found, running in direct mode"
    export GRAMINE_SGX_MODE=0
    export GRAMINE_DIRECT_MODE=1
fi

echo ""
echo "Gramine version:"
gramine-sgx --version 2>/dev/null || gramine-direct --version || echo "Gramine not found in PATH"

echo ""
echo "Node.js version:"
node --version

echo ""
echo "Available Web3 tools:"
which web3 >/dev/null 2>&1 && echo "  - web3" || true
which hardhat >/dev/null 2>&1 && echo "  - hardhat" || true
which truffle >/dev/null 2>&1 && echo "  - truffle" || true
which ganache >/dev/null 2>&1 && echo "  - ganache" || true

echo ""
echo "=========================================="
echo "Container ready!"
echo "=========================================="
echo ""

exec "$@"
