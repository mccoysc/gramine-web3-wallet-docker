# MySQL 8 with RA-TLS (Remote Attestation TLS) Support in SGX Enclave
#
# This Dockerfile creates a MySQL 8 server image that runs inside an SGX enclave
# with transparent RA-TLS support using the gramine-web3-wallet-docker base image.
#
# Architecture:
# - A C launcher program runs inside the SGX enclave
# - The launcher reads whitelist from smart contract (if configured)
# - The launcher uses execve() to replace itself with mysqld (same enclave)
# - RA-TLS is injected via LD_PRELOAD configured in the Gramine manifest
# - DCAP attestation is enabled for remote verification
#
# Features:
# - Mutual RA-TLS authentication (both client and server must be SGX-based)
# - Certificate-only MySQL authentication (no passwords)
# - Certificates generated at startup from SGX quotes using secp256k1 curve
# - Optional whitelist from smart contract (via CONTRACT_ADDRESS env var)
# - Pre-compiled Gramine manifest for immediate SGX execution
#
# Environment Variables:
#   CONTRACT_ADDRESS - Smart contract address to read whitelist from (optional)
#   RPC_URL - Ethereum RPC endpoint URL (required if CONTRACT_ADDRESS is set)
#   RATLS_WHITELIST_CONFIG - Manual whitelist override (optional)
#   MYSQL_ROOT_PASSWORD - MySQL root password (optional, cert auth preferred)
#   MYSQL_DATABASE - Database to create on first run (optional)
#   MYSQL_USER - User to create on first run (optional)
#
# Usage:
#   docker build -t mysql-ratls .
#   docker run -d \
#     --device=/dev/sgx_enclave \
#     --device=/dev/sgx_provision \
#     -e PCCS_API_KEY=your_api_key \
#     -e CONTRACT_ADDRESS=0x... \
#     -e RPC_URL=https://... \
#     -p 3306:3306 \
#     mysql-ratls
#

ARG BASE_IMAGE=ghcr.io/mccoysc/gramine-web3-wallet-docker:latest
FROM ${BASE_IMAGE}

LABEL org.opencontainers.image.description="MySQL 8 with RA-TLS (Remote Attestation TLS) support for SGX-based mutual authentication"
LABEL org.opencontainers.image.licenses=GPL-2.0

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install MySQL 8 server and development dependencies for launcher compilation
RUN apt-get update && apt-get install -y --no-install-recommends \
    mysql-server \
    mysql-client \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Create directories for MySQL data and SSL certificates
RUN mkdir -p /var/lib/mysql /var/lib/mysql-ssl /var/run/mysqld \
    && chown -R mysql:mysql /var/lib/mysql /var/lib/mysql-ssl /var/run/mysqld \
    && chmod 755 /var/run/mysqld

# Copy the C launcher source code and build files
COPY launcher/ /tmp/mysql-ratls-launcher/
COPY mysql-ratls.manifest.template /tmp/mysql-ratls-launcher/
COPY build-gramine-manifest.sh /tmp/mysql-ratls-launcher/

# Compile the C launcher
RUN cd /tmp/mysql-ratls-launcher && \
    make clean || true && \
    make && \
    make install && \
    rm -rf /tmp/mysql-ratls-launcher

# Copy the manifest template and build script to the examples directory
COPY mysql-ratls.manifest.template /opt/mysql-ratls/
COPY build-gramine-manifest.sh /opt/mysql-ratls/
RUN chmod +x /opt/mysql-ratls/build-gramine-manifest.sh

# Configure MySQL for certificate-only authentication
# This configuration enforces SSL/TLS and prepares for RA-TLS certificates
RUN cat >> /etc/mysql/mysql.conf.d/mysqld.cnf <<'EOF'

# RA-TLS Configuration
[mysqld]
# Require secure transport (SSL/TLS)
require_secure_transport = ON

# SSL certificate paths (generated at runtime by RA-TLS)
# Certificates are in normal directory, private key is in encrypted partition
ssl_ca = /var/lib/mysql-ssl/ca.pem
ssl_cert = /var/lib/mysql-ssl/server-cert.pem
# Private key path is set via command line argument (from encrypted partition)

# Require client certificates for authentication
ssl_mode = VERIFY_IDENTITY

# Bind to all interfaces
bind-address = 0.0.0.0

# Skip name resolution for faster connections
skip-name-resolve

# Performance settings
max_connections = 100
innodb_buffer_pool_size = 256M

# Data and log directories in encrypted partition to prevent data leakage
# All sensitive data MUST be stored in encrypted partition
datadir = /app/wallet/mysql-data
log_error = /app/wallet/mysql-logs/error.log
general_log_file = /app/wallet/mysql-logs/general.log
slow_query_log_file = /app/wallet/mysql-logs/slow.log

EOF

# Build the Gramine manifest (pre-compile during Docker build)
# This generates the signed manifest so it's ready to run immediately
# Note: MySQL data, logs, and private keys are in encrypted partition (/app/wallet/)
RUN mkdir -p /var/lib/mysql-ssl /var/run/mysqld \
    /app/wallet/mysql-keys /app/wallet/mysql-data /app/wallet/mysql-logs && \
    chown -R mysql:mysql /app/wallet/mysql-data /app/wallet/mysql-logs && \
    cd /opt/mysql-ratls && \
    ./build-gramine-manifest.sh

# Set environment variables for RA-TLS
# Use secp256k1 curve for Ethereum compatibility
ENV RA_TLS_CERT_ALGORITHM=secp256k1
ENV RATLS_ENABLE_VERIFY=1
ENV RATLS_REQUIRE_PEER_CERT=1

# MySQL data and logs directories MUST be in encrypted partition to prevent data leakage
ENV MYSQL_DATA_DIR=/app/wallet/mysql-data
ENV MYSQL_LOG_DIR=/app/wallet/mysql-logs
# RA-TLS certificate and key paths (used by libratls-quote-verify.so and MySQL)
# Certificate can be in normal directory
ENV RATLS_CERT_PATH=/var/lib/mysql-ssl/server-cert.pem
# Private key MUST be in encrypted partition for security
ENV RATLS_KEY_PATH=/app/wallet/mysql-keys/server-key.pem

# Expose MySQL port
EXPOSE 3306

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD mysqladmin ping -h localhost --silent || exit 1

# Set working directory to encrypted MySQL data directory
WORKDIR /app/wallet/mysql-data

# The entrypoint runs the base image's entrypoint first (starts aesmd, PCCS, etc.)
# Then runs gramine-sgx with the pre-compiled manifest to start the launcher in SGX enclave
# The launcher (inside enclave) will then execve() to mysqld
ENTRYPOINT ["/usr/local/bin/entrypoint.sh", "gramine-sgx", "mysql-ratls"]
CMD []
