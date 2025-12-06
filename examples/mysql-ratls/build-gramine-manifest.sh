#!/bin/bash
# Build script for MySQL RA-TLS Gramine manifest
# This script is run during Docker build to pre-compile the manifest

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DIR="${SCRIPT_DIR}/launcher"
MANIFEST_TEMPLATE="${SCRIPT_DIR}/mysql-ratls.manifest.template"
OUTPUT_DIR="/var/lib/mysql"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Step 1: Compile the launcher
log_info "Compiling MySQL RA-TLS launcher..."
cd "${LAUNCHER_DIR}"
make clean || true
make

# Install the launcher
log_info "Installing launcher to /usr/local/bin..."
make install

# Verify the launcher was installed
if [ ! -x /usr/local/bin/mysql-ratls-launcher ]; then
    log_error "Launcher installation failed!"
    exit 1
fi

log_info "Launcher compiled and installed successfully"

# Step 2: Create output directory
log_info "Creating output directory: ${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

# Step 3: Find RA-TLS library path
RATLS_LIB_PATH=""
for path in /usr/local/lib/x86_64-linux-gnu/libratls-quote-verify.so \
            /usr/local/lib/libratls-quote-verify.so \
            /usr/lib/x86_64-linux-gnu/libratls-quote-verify.so; do
    if [ -f "$path" ]; then
        RATLS_LIB_PATH="$path"
        break
    fi
done

if [ -z "$RATLS_LIB_PATH" ]; then
    log_warn "libratls-quote-verify.so not found, RA-TLS injection may not work"
else
    log_info "Found RA-TLS library: ${RATLS_LIB_PATH}"
fi

# Step 4: Generate the manifest
log_info "Generating Gramine manifest..."
cd "${OUTPUT_DIR}"

# Set GRAMINE_LD_PRELOAD for automatic RA-TLS injection
if [ -n "$RATLS_LIB_PATH" ]; then
    export GRAMINE_LD_PRELOAD="file:${RATLS_LIB_PATH}"
    log_info "Set GRAMINE_LD_PRELOAD=${GRAMINE_LD_PRELOAD}"
fi

# Generate manifest from template
gramine-manifest \
    -Dentrypoint=/usr/local/bin/mysql-ratls-launcher \
    -Dlog_level=error \
    "${MANIFEST_TEMPLATE}" \
    "${OUTPUT_DIR}/mysql-ratls.manifest"

if [ ! -f "${OUTPUT_DIR}/mysql-ratls.manifest" ]; then
    log_error "Failed to generate manifest!"
    exit 1
fi

log_info "Manifest generated: ${OUTPUT_DIR}/mysql-ratls.manifest"

# Step 5: Sign the manifest
log_info "Signing manifest with gramine-sgx-sign..."

# Generate a signing key if it doesn't exist
SIGNING_KEY="/root/.config/gramine/enclave-key.pem"
if [ ! -f "$SIGNING_KEY" ]; then
    log_info "Generating signing key..."
    mkdir -p "$(dirname "$SIGNING_KEY")"
    gramine-sgx-gen-private-key
fi

gramine-sgx-sign \
    --manifest "${OUTPUT_DIR}/mysql-ratls.manifest" \
    --output "${OUTPUT_DIR}/mysql-ratls.manifest.sgx"

if [ ! -f "${OUTPUT_DIR}/mysql-ratls.manifest.sgx" ]; then
    log_error "Failed to sign manifest!"
    exit 1
fi

log_info "Signed manifest: ${OUTPUT_DIR}/mysql-ratls.manifest.sgx"

# Step 6: Verify the files
log_info "Verifying generated files..."
ls -la "${OUTPUT_DIR}"/mysql-ratls.manifest*

# Also copy manifest to a backup location for reference
cp "${OUTPUT_DIR}/mysql-ratls.manifest" "${SCRIPT_DIR}/"
cp "${OUTPUT_DIR}/mysql-ratls.manifest.sgx" "${SCRIPT_DIR}/"

log_info "Build complete!"
log_info ""
log_info "To run MySQL in SGX enclave:"
log_info "  cd ${OUTPUT_DIR}"
log_info "  gramine-sgx mysql-ratls"
